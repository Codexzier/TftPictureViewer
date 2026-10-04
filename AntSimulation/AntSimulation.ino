/*
  Ant Simulation

  Hardware:
    - Arduino UNO R3
    - 1.8" TFT-Shield (ST7735, 160x128)

  Ablauf:
    - Gruene Flaeche, darauf an zufaelliger Stelle eine braune Ameisenkolonie
      (Radius 12 Pixel).
    - Jede Sekunde schluepft eine Ameise (1 schwarzer Pixel), hoechstens 20
      gleichzeitig. Sie laeuft geradeaus in eine zufaellige Richtung und waehlt
      am Bildschirmrand eine neue, moegliche Richtung.
    - Zufaellig erscheint ein weisses Zuckerstueck (3 Pixel). Findet eine
      Ameise den Zucker, sendet sie ein Signal im Umkreis von 8 Pixel. Jede
      Ameise, die es empfaengt, laeuft zum Zucker und hilft tragen. Je mehr
      Ameisen tragen, desto schneller wandert der Zucker zur Kolonie.
    - Erreicht der Zucker die Kolonie, verschwinden Zucker und Traeger darin,
      der Punktestand oben rechts steigt um eins. 3 s spaeter erscheint neuer
      Zucker an einer freien Stelle.
    - Oben links steht die Anzahl der geschluepften Ameisen.

  Bibliotheken:
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ---------------------------------------------------------------------------
// Pins und Display (TFT-Shield)
// ---------------------------------------------------------------------------
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

#define TFT_TAB       INITR_BLACKTAB
#define TFT_ROTATION  1               // Querformat 160x128

const int16_t SCREEN_W = 160;
const int16_t SCREEN_H = 128;
const int16_t FIELD_Y0 = 10;          // darueber die Textzeile

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------
const uint8_t  COLONY_R        = 12;      // Radius der Kolonie
const uint8_t  MAX_ANTS        = 20;      // gleichzeitig unterwegs
const uint16_t SPAWN_MS        = 1000;    // eine neue Ameise pro Sekunde
const uint8_t  SIGNAL_R        = 8;       // Reichweite des Zucker-Signals
const uint16_t SUGAR_DELAY_MS  = 3000;    // bis zum neuen Zucker
const uint8_t  TICK_MS         = 30;      // Simulationsschritt

// Geschwindigkeiten in 1/256 Pixel pro Schritt
const uint8_t  ANT_SPEED       = 127;     // ~0,5 px pro Schritt = ~16 px/s
const uint8_t  CARRY_SPEED     = 24;      // je tragender Ameise
const uint8_t  MAX_CARRIERS    = 10;      // mehr bringen keine Beschleunigung

const uint16_t COLOR_GRASS  = 0x2C64;    // gruen
const uint16_t COLOR_COLONY = 0x8A82;    // braun
const uint16_t COLOR_SUGAR  = ST77XX_WHITE;
const uint16_t COLOR_ANT    = ST77XX_BLACK;
const uint16_t COLOR_TEXT   = ST77XX_WHITE;

// 32 Laufrichtungen (cos/sin * 127)
const int8_t DIRS[32][2] PROGMEM = {
  {127,0}, {125,25}, {117,49}, {106,71}, {90,90}, {71,106}, {49,117}, {25,125},
  {0,127}, {-25,125}, {-49,117}, {-71,106}, {-90,90}, {-106,71}, {-117,49}, {-125,25},
  {-127,0}, {-125,-25}, {-117,-49}, {-106,-71}, {-90,-90}, {-71,-106}, {-49,-117}, {-25,-125},
  {0,-127}, {25,-125}, {49,-117}, {71,-106}, {90,-90}, {106,-71}, {117,-49}, {125,-25}
};

// Plaetze der Traeger rund um den Zucker (Abstand 2 Pixel)
const int8_t RING[16][2] PROGMEM = {
  {-2,-2}, {0,-2}, {2,-2}, {2,0}, {2,2}, {0,2}, {-2,2}, {-2,0},
  {-1,-2}, {1,-2}, {2,-1}, {2,1}, {1,2}, {-1,2}, {-2,1}, {-2,-1}
};

// ---------------------------------------------------------------------------
// Zustaende
// ---------------------------------------------------------------------------
enum AntState : uint8_t { ANT_FREE, ANT_WANDER, ANT_RECRUITED, ANT_CARRY };

struct Ant {
  int32_t  x, y;          // Position in 1/256 Pixel
  int16_t  vx, vy;        // Laufrichtung in 1/256 Pixel pro Schritt
  AntState state;
  uint8_t  slot;          // Platz am Zucker beim Tragen
  uint8_t  px, py;        // zuletzt gezeichneter Pixel
  bool     drawn;
};

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

Ant      ants[MAX_ANTS];
int16_t  colonyX, colonyY;

bool     sugarActive = false;
int32_t  sugarX, sugarY;          // 1/256 Pixel
int16_t  sugarPx = -1, sugarPy;   // zuletzt gezeichnet
uint8_t  carriers = 0;
uint32_t nextSugarMs = 0;

uint16_t spawnCount = 0;
uint16_t score = 0;
uint32_t lastSpawnMs = 0;
uint32_t lastTickMs = 0;

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
bool inColony(int16_t x, int16_t y, uint8_t r) {
  int16_t dx = x - colonyX;
  int16_t dy = y - colonyY;
  return (int32_t)dx * dx + (int32_t)dy * dy <= (int32_t)r * r;
}

// Farbe des Untergrunds (ohne Zucker und Ameisen)
uint16_t groundColor(int16_t x, int16_t y) {
  return inColony(x, y, COLONY_R) ? COLOR_COLONY : COLOR_GRASS;
}

bool inField(int32_t x, int32_t y) {
  return x >= 0 && x < ((int32_t)SCREEN_W << 8) &&
         y >= ((int32_t)FIELD_Y0 << 8) && y < ((int32_t)SCREEN_H << 8);
}

void setDirection(Ant &a, uint8_t dir) {
  a.vx = (int8_t)pgm_read_byte(&DIRS[dir][0]) * ANT_SPEED / 127;
  a.vy = (int8_t)pgm_read_byte(&DIRS[dir][1]) * ANT_SPEED / 127;
}

// Neue zufaellige Richtung, mit der die Ameise im Bild bleibt
void turnInside(Ant &a) {
  for (uint8_t tries = 0; tries < 40; tries++) {
    setDirection(a, random(32));
    if (inField(a.x + a.vx, a.y + a.vy)) return;
  }
  a.vx = -a.vx;
  a.vy = -a.vy;
}

// Schritt in Richtung Ziel mit Ameisengeschwindigkeit
void stepTowards(Ant &a, int32_t tx, int32_t ty) {
  int32_t dx = tx - a.x;
  int32_t dy = ty - a.y;
  int32_t m = max(abs(dx), abs(dy));
  if (m <= ANT_SPEED) {
    a.x = tx;
    a.y = ty;
    return;
  }
  a.x += dx * ANT_SPEED / m;
  a.y += dy * ANT_SPEED / m;
}

void drawHud() {
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT, COLOR_GRASS);
  tft.setCursor(1, 1);
  tft.print(F("Ameisen:"));
  tft.print(spawnCount);

  char buf[16];
  uint8_t len = snprintf(buf, sizeof(buf), " Punkte:%u", score);
  tft.setCursor(SCREEN_W - 1 - len * 6, 1);
  tft.print(buf);
}

// ---------------------------------------------------------------------------
// Aufbau
// ---------------------------------------------------------------------------
void drawColony() {
  // gleiche Kreisformel wie groundColor(), damit das Wiederherstellen passt
  for (int16_t dy = -COLONY_R; dy <= COLONY_R; dy++) {
    int16_t w = 0;
    while ((int32_t)(w + 1) * (w + 1) + (int32_t)dy * dy <= (int32_t)COLONY_R * COLONY_R) w++;
    tft.drawFastHLine(colonyX - w, colonyY + dy, 2 * w + 1, COLOR_COLONY);
  }
}

void spawnSugar() {
  for (uint8_t tries = 0; tries < 100; tries++) {
    int16_t x = random(4, SCREEN_W - 4);
    int16_t y = random(FIELD_Y0 + 4, SCREEN_H - 4);
    if (inColony(x, y, COLONY_R + 10)) continue;   // nicht direkt an der Kolonie
    sugarX = (int32_t)x << 8;
    sugarY = (int32_t)y << 8;
    sugarActive = true;
    carriers = 0;
    sugarPx = -1;
    return;
  }
}

void setup() {
  tft.initR(TFT_TAB);
  tft.setRotation(TFT_ROTATION);
  tft.setTextWrap(false);
  randomSeed(analogRead(A0) ^ micros());

  tft.fillScreen(COLOR_GRASS);
  colonyX = random(COLONY_R + 2, SCREEN_W - COLONY_R - 2);
  colonyY = random(FIELD_Y0 + COLONY_R + 2, SCREEN_H - COLONY_R - 2);
  drawColony();

  for (uint8_t i = 0; i < MAX_ANTS; i++) ants[i].state = ANT_FREE;
  spawnSugar();
  drawHud();
  lastSpawnMs = lastTickMs = millis();
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
void spawnAnt() {
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state != ANT_FREE) continue;
    a.x = (int32_t)colonyX << 8;
    a.y = (int32_t)colonyY << 8;
    a.state = ANT_WANDER;
    a.drawn = false;
    turnInside(a);
    spawnCount++;
    drawHud();
    return;
  }
}

// Zucker ist in der Kolonie: Punkt, Traeger verschwinden, Helfer laufen weiter
void deliverSugar() {
  sugarActive = false;
  score++;
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state == ANT_CARRY) {
      if (a.drawn) tft.drawPixel(a.px, a.py, groundColor(a.px, a.py));
      a.state = ANT_FREE;
    } else if (a.state == ANT_RECRUITED) {
      a.state = ANT_WANDER;
      turnInside(a);
    }
  }
  carriers = 0;
  nextSugarMs = millis() + SUGAR_DELAY_MS;
  drawHud();
}

// Ameise packt mit an: fester Platz rund um den Zucker
void becomeCarrier(Ant &a) {
  a.state = ANT_CARRY;
  a.slot = carriers++;
  a.x = sugarX + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][0]) << 8);
  a.y = sugarY + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][1]) << 8);
}

// Abstand in ganzen Pixeln, genau wie gezeichnet
int32_t sugarDist2(const Ant &a) {
  int32_t dx = (sugarX >> 8) - (a.x >> 8);
  int32_t dy = (sugarY >> 8) - (a.y >> 8);
  return dx * dx + dy * dy;
}

void updateAnt(Ant &a) {
  int32_t d2 = sugarDist2(a);

  switch (a.state) {
    case ANT_WANDER:
      if (sugarActive && carriers > 0 && d2 <= (int32_t)SIGNAL_R * SIGNAL_R) {
        a.state = ANT_RECRUITED;                   // Signal empfangen
        return;
      }
      if (!inField(a.x + a.vx, a.y + a.vy)) turnInside(a);
      a.x += a.vx;
      a.y += a.vy;
      if (sugarActive && carriers < 16 && sugarDist2(a) <= 4) {
        becomeCarrier(a);                          // Zucker gefunden
      }
      break;

    case ANT_RECRUITED:
      if (carriers >= 16 && d2 <= 9) break;        // alle Plaetze belegt: daneben warten
      stepTowards(a, sugarX, sugarY);
      if (sugarDist2(a) <= 4 && carriers < 16) becomeCarrier(a);
      break;

    case ANT_CARRY:
      a.x = sugarX + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][0]) << 8);
      a.y = sugarY + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][1]) << 8);
      break;

    default:
      break;
  }
}

void moveSugar() {
  if (!sugarActive || carriers == 0) return;
  uint8_t helpers = carriers < MAX_CARRIERS ? carriers : MAX_CARRIERS;
  int32_t speed = (int32_t)helpers * CARRY_SPEED;
  int32_t dx = ((int32_t)colonyX << 8) - sugarX;
  int32_t dy = ((int32_t)colonyY << 8) - sugarY;
  int32_t m = max(abs(dx), abs(dy));
  if (m <= speed) {
    sugarX += dx;
    sugarY += dy;
  } else {
    sugarX += dx * speed / m;
    sugarY += dy * speed / m;
  }
}

void tick() {
  uint32_t now = millis();

  // Neue Ameise
  if (now - lastSpawnMs >= SPAWN_MS) {
    lastSpawnMs += SPAWN_MS;
    spawnAnt();
  }
  // Neuer Zucker
  if (!sugarActive && (int32_t)(now - nextSugarMs) >= 0) spawnSugar();

  moveSugar();
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    if (ants[i].state != ANT_FREE) updateAnt(ants[i]);
  }

  // Zucker an der alten Stelle loeschen
  int16_t spx = sugarX >> 8, spy = sugarY >> 8;
  if (sugarPx >= 0 && (!sugarActive || spx != sugarPx || spy != sugarPy)) {
    for (int8_t oy = -1; oy <= 1; oy++) {
      for (int8_t ox = -1; ox <= 1; ox++) {
        tft.drawPixel(sugarPx + ox, sugarPy + oy, groundColor(sugarPx + ox, sugarPy + oy));
      }
    }
    sugarPx = -1;
  }

  // Ameisen an der alten Stelle loeschen
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state == ANT_FREE || !a.drawn) continue;
    if (a.px != (a.x >> 8) || a.py != (a.y >> 8)) {
      tft.drawPixel(a.px, a.py, groundColor(a.px, a.py));
    }
  }

  // Zucker in der Kolonie angekommen?
  if (sugarActive && inColony(spx, spy, COLONY_R - 4)) {
    deliverSugar();
  }

  // Zucker und Ameisen zeichnen
  if (sugarActive) {
    tft.fillRect(spx - 1, spy - 1, 3, 3, COLOR_SUGAR);
    sugarPx = spx;
    sugarPy = spy;
  }
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state == ANT_FREE) continue;
    a.px = a.x >> 8;
    a.py = a.y >> 8;
    a.drawn = true;
    tft.drawPixel(a.px, a.py, COLOR_ANT);
  }
}

void loop() {
  if (millis() - lastTickMs < TICK_MS) return;
  lastTickMs += TICK_MS;
  tick();
}
