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
    - Die Kolonie ist fuer umherlaufende Ameisen ein Hindernis wie der
      Bildschirmrand; nur Zuckertraeger gehen hinein.
    - 30 s nach dem Start (und 30 s nach jeder Niederlage) erscheint am Rand ein
      blauer Kaefer (3 Pixel). Trifft er eine Ameise, haelt sie 2 s durch,
      macht 1/10 Schaden und wird dann gefressen - ausser ihr Treffer besiegt
      den Kaefer. Waehrend des Kampfes sendet sie ein Signal im Umkreis von
      20 Pixel, alle Ameisen darin greifen mit an. Ein besiegter Kaefer bringt
      3 Punkte. Der Kaefer heilt alle 10 s um 1/10; frisst er Zucker, hat er
      sofort wieder volle Lebensenergie.

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

const uint32_t BEETLE_DELAY_MS = 30000;   // bis zum (naechsten) Kaefer
const uint8_t  BEETLE_HP       = 10;      // Lebensenergie in Zehnteln
const uint8_t  FIGHT_TICKS     = 2000 / TICK_MS;   // 2 s Kampf je Ameise
const uint16_t REGEN_MS        = 10000;   // Kaefer heilt 1/10 alle 10 s
const uint8_t  ALARM_R         = 20;      // Reichweite des Kampf-Signals
const uint8_t  BEETLE_POINTS   = 3;

// Geschwindigkeiten in 1/256 Pixel pro Schritt
const uint8_t  ANT_SPEED       = 127;     // ~0,5 px pro Schritt = ~16 px/s
const uint8_t  CARRY_SPEED     = 24;      // je tragender Ameise
const uint8_t  MAX_CARRIERS    = 10;      // mehr bringen keine Beschleunigung
const uint8_t  BEETLE_SPEED    = 80;      // ~10 px/s

const uint16_t COLOR_GRASS  = 0x2C64;    // gruen
const uint16_t COLOR_COLONY = 0x8A82;    // braun
const uint16_t COLOR_SUGAR  = ST77XX_WHITE;
const uint16_t COLOR_ANT    = ST77XX_BLACK;
const uint16_t COLOR_TEXT   = ST77XX_WHITE;
const uint16_t COLOR_BEETLE = 0x335F;    // blau

// 32 Laufrichtungen (cos/sin * 127)
const int8_t DIRS[32][2] PROGMEM = {
  {127,0}, {125,25}, {117,49}, {106,71}, {90,90}, {71,106}, {49,117}, {25,125},
  {0,127}, {-25,125}, {-49,117}, {-71,106}, {-90,90}, {-106,71}, {-117,49}, {-125,25},
  {-127,0}, {-125,-25}, {-117,-49}, {-106,-71}, {-90,-90}, {-71,-106}, {-49,-117}, {-25,-125},
  {0,-127}, {25,-125}, {49,-117}, {71,-106}, {90,-90}, {106,-71}, {117,-49}, {125,-25}
};

// Plaetze rund um Zucker bzw. Kaefer (Abstand 2 Pixel)
const int8_t RING[16][2] PROGMEM = {
  {-2,-2}, {0,-2}, {2,-2}, {2,0}, {2,2}, {0,2}, {-2,2}, {-2,0},
  {-1,-2}, {1,-2}, {2,-1}, {2,1}, {1,2}, {-1,2}, {-2,1}, {-2,-1}
};

// ---------------------------------------------------------------------------
// Zustaende
// ---------------------------------------------------------------------------
enum AntState : uint8_t {
  ANT_FREE,        // nicht unterwegs
  ANT_WANDER,      // laeuft geradeaus
  ANT_RECRUITED,   // Zucker-Signal: laeuft zum Zucker
  ANT_CARRY,       // traegt Zucker
  ANT_TO_BEETLE,   // Kampf-Signal: laeuft zum Kaefer
  ANT_FIGHT        // kaempft am Kaefer
};

struct Ant {
  int32_t  x, y;          // Position in 1/256 Pixel
  int16_t  vx, vy;        // Laufrichtung in 1/256 Pixel pro Schritt
  AntState state;
  uint8_t  slot;          // Platz am Zucker bzw. Kaefer
  uint8_t  timer;         // verbleibende Kampfschritte
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

bool     beetleActive = false;
int32_t  beetleX, beetleY;        // 1/256 Pixel
int16_t  beetleVx, beetleVy;
int16_t  beetlePx = -1, beetlePy; // zuletzt gezeichnet
uint8_t  beetleHp = 0;
uint16_t beetleSlots = 0;         // belegte Kampfplaetze (Bitmaske)
uint32_t nextBeetleMs = 0;
uint32_t lastRegenMs = 0;

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

// Darf eine Ameise hier stehen? (im Feld und nicht in der Kolonie)
bool antFree(int32_t x, int32_t y) {
  return inField(x, y) && !inColony(x >> 8, y >> 8, COLONY_R);
}

// Darf der Kaefer (3x3) hier stehen?
bool beetleFree(int32_t x, int32_t y) {
  int16_t px = x >> 8, py = y >> 8;
  return px >= 1 && px <= SCREEN_W - 2 && py >= FIELD_Y0 + 1 && py <= SCREEN_H - 2 &&
         !inColony(px, py, COLONY_R + 2);
}

void dirVector(uint8_t dir, uint8_t speed, int16_t &vx, int16_t &vy) {
  vx = (int8_t)pgm_read_byte(&DIRS[dir][0]) * speed / 127;
  vy = (int8_t)pgm_read_byte(&DIRS[dir][1]) * speed / 127;
}

// Neue zufaellige Richtung, mit der die Ameise im Bild und ausserhalb der
// Kolonie bleibt
void turnInside(Ant &a) {
  for (uint8_t tries = 0; tries < 40; tries++) {
    dirVector(random(32), ANT_SPEED, a.vx, a.vy);
    if (antFree(a.x + a.vx, a.y + a.vy)) return;
  }
  a.vx = -a.vx;
  a.vy = -a.vy;
}

// Schritt in Richtung Ziel mit Ameisengeschwindigkeit, um die Kolonie herum
void stepTowards(Ant &a, int32_t tx, int32_t ty) {
  int32_t dx = tx - a.x;
  int32_t dy = ty - a.y;
  int32_t m = max(abs(dx), abs(dy));
  if (m == 0) return;
  int32_t sx = m <= ANT_SPEED ? dx : dx * ANT_SPEED / m;
  int32_t sy = m <= ANT_SPEED ? dy : dy * ANT_SPEED / m;
  if (antFree(a.x + sx, a.y + sy)) {
    a.x += sx;
    a.y += sy;
  } else if (sx != 0 && antFree(a.x + (sx > 0 ? ANT_SPEED : -ANT_SPEED), a.y)) {
    a.x += sx > 0 ? ANT_SPEED : -ANT_SPEED;   // an der Kolonie entlang
  } else if (sy != 0 && antFree(a.x, a.y + (sy > 0 ? ANT_SPEED : -ANT_SPEED))) {
    a.y += sy > 0 ? ANT_SPEED : -ANT_SPEED;
  }
}

// Abstand in ganzen Pixeln (Schachbrett-Abstand), genau wie gezeichnet
int16_t pixelDist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  int16_t dx = abs((int16_t)(ax >> 8) - (int16_t)(bx >> 8));
  int16_t dy = abs((int16_t)(ay >> 8) - (int16_t)(by >> 8));
  return max(dx, dy);
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
  nextBeetleMs = lastSpawnMs + BEETLE_DELAY_MS;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
void spawnAnt() {
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state != ANT_FREE) continue;
    // Schluepfen am Rand der Kolonie, Blick nach aussen
    bool placed = false;
    for (uint8_t tries = 0; tries < 40 && !placed; tries++) {
      uint8_t dir = random(32);
      int16_t ox, oy;
      dirVector(dir, COLONY_R + 2, ox, oy);
      a.x = (int32_t)(colonyX + ox) << 8;
      a.y = (int32_t)(colonyY + oy) << 8;
      dirVector(dir, ANT_SPEED, a.vx, a.vy);
      placed = antFree(a.x, a.y);
    }
    if (!placed) return;
    a.state = ANT_WANDER;
    a.drawn = false;
    if (!antFree(a.x + a.vx, a.y + a.vy)) turnInside(a);
    spawnCount++;
    drawHud();
    return;
  }
}

void eraseAnt(Ant &a) {
  if (a.drawn) tft.drawPixel(a.px, a.py, groundColor(a.px, a.py));
  a.state = ANT_FREE;
}

// Zucker verschwindet: in der Kolonie (Punkt, Traeger gehen mit hinein) oder
// vom Kaefer gefressen (Traeger laufen weiter)
void removeSugar(bool delivered) {
  sugarActive = false;
  if (delivered) score++;
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state == ANT_CARRY && delivered) {
      eraseAnt(a);
    } else if (a.state == ANT_CARRY || a.state == ANT_RECRUITED) {
      a.state = ANT_WANDER;
      turnInside(a);
    }
  }
  carriers = 0;
  nextSugarMs = millis() + SUGAR_DELAY_MS;
  drawHud();
}

// ---------------------------------------------------------------------------
// Kaefer
// ---------------------------------------------------------------------------
void spawnBeetle() {
  for (uint8_t tries = 0; tries < 50; tries++) {
    int16_t x, y;
    switch (random(4)) {   // zufaelliger Bildschirmrand
      case 0:  x = 1;            y = random(FIELD_Y0 + 1, SCREEN_H - 1); break;
      case 1:  x = SCREEN_W - 2; y = random(FIELD_Y0 + 1, SCREEN_H - 1); break;
      case 2:  x = random(1, SCREEN_W - 1); y = FIELD_Y0 + 1;           break;
      default: x = random(1, SCREEN_W - 1); y = SCREEN_H - 2;           break;
    }
    beetleX = (int32_t)x << 8;
    beetleY = (int32_t)y << 8;
    if (!beetleFree(beetleX, beetleY)) continue;
    beetleActive = true;
    beetleHp = BEETLE_HP;
    beetleSlots = 0;
    beetlePx = -1;
    lastRegenMs = millis();
    beetleVx = beetleVy = 0;
    return;
  }
}

void turnBeetle() {
  for (uint8_t tries = 0; tries < 40; tries++) {
    dirVector(random(32), BEETLE_SPEED, beetleVx, beetleVy);
    if (beetleFree(beetleX + beetleVx, beetleY + beetleVy)) return;
  }
  beetleVx = beetleVy = 0;
}

void moveBeetle() {
  if (!beetleActive || beetleSlots != 0) return;   // kaempft: bleibt stehen
  if ((beetleVx == 0 && beetleVy == 0) ||
      !beetleFree(beetleX + beetleVx, beetleY + beetleVy)) turnBeetle();
  beetleX += beetleVx;
  beetleY += beetleVy;
}

// Kaefer besiegt: Punkte, Kaempfer ueberleben
void defeatBeetle() {
  beetleActive = false;
  beetleSlots = 0;
  score += BEETLE_POINTS;
  for (uint8_t i = 0; i < MAX_ANTS; i++) {
    Ant &a = ants[i];
    if (a.state == ANT_FIGHT || a.state == ANT_TO_BEETLE) {
      a.state = ANT_WANDER;
      turnInside(a);
    }
  }
  nextBeetleMs = millis() + BEETLE_DELAY_MS;
  drawHud();
}

// Freien Kampfplatz am Kaefer belegen
bool joinFight(Ant &a) {
  for (uint8_t s = 0; s < 16; s++) {
    if (beetleSlots & (1U << s)) continue;
    int32_t x = beetleX + ((int32_t)(int8_t)pgm_read_byte(&RING[s][0]) << 8);
    int32_t y = beetleY + ((int32_t)(int8_t)pgm_read_byte(&RING[s][1]) << 8);
    if (!antFree(x, y)) continue;                  // Platz ausserhalb des Feldes
    beetleSlots |= 1U << s;
    a.state = ANT_FIGHT;
    a.slot = s;
    a.timer = FIGHT_TICKS;
    a.x = x;
    a.y = y;
    return true;
  }
  return false;
}

// Kampf einer Ameise: nach 2 s 1/10 Schaden, dann gefressen
void updateFighter(Ant &a) {
  if (--a.timer > 0) return;
  beetleHp--;
  if (beetleHp == 0) {
    defeatBeetle();
    return;
  }
  beetleSlots &= ~(1U << a.slot);
  eraseAnt(a);
}

// Prueft fuer eine laufende Ameise Kontakt bzw. Signal vom Kaefer.
// Liefert true, wenn sich der Zustand geaendert hat.
bool checkBeetle(Ant &a) {
  if (!beetleActive) return false;
  int16_t d = pixelDist(a.x, a.y, beetleX, beetleY);
  if (d <= 2) return joinFight(a);                 // Kaefer trifft Ameise
  if (beetleSlots != 0 && a.state != ANT_TO_BEETLE) {
    int32_t dx = (beetleX >> 8) - (a.x >> 8);
    int32_t dy = (beetleY >> 8) - (a.y >> 8);
    if (dx * dx + dy * dy <= (int32_t)ALARM_R * ALARM_R) {
      a.state = ANT_TO_BEETLE;                     // Kampf-Signal empfangen
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Zucker
// ---------------------------------------------------------------------------
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

  if ((a.state == ANT_WANDER || a.state == ANT_RECRUITED) && checkBeetle(a)) return;

  switch (a.state) {
    case ANT_WANDER:
      if (sugarActive && carriers > 0 && d2 <= (int32_t)SIGNAL_R * SIGNAL_R) {
        a.state = ANT_RECRUITED;                   // Signal empfangen
        return;
      }
      if (!antFree(a.x + a.vx, a.y + a.vy)) turnInside(a);
      a.x += a.vx;
      a.y += a.vy;
      if (checkBeetle(a)) break;
      if (sugarActive && carriers < 16 && sugarDist2(a) <= 4) {
        becomeCarrier(a);                          // Zucker gefunden
      }
      break;

    case ANT_RECRUITED:
      if (carriers >= 16 && d2 <= 9) break;        // alle Plaetze belegt: daneben warten
      stepTowards(a, sugarX, sugarY);
      if (checkBeetle(a)) break;
      if (sugarDist2(a) <= 4 && carriers < 16) becomeCarrier(a);
      break;

    case ANT_CARRY:
      a.x = sugarX + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][0]) << 8);
      a.y = sugarY + ((int32_t)(int8_t)pgm_read_byte(&RING[a.slot][1]) << 8);
      break;

    case ANT_TO_BEETLE:
      if (!beetleActive) {
        a.state = ANT_WANDER;
        turnInside(a);
        break;
      }
      if (pixelDist(a.x, a.y, beetleX, beetleY) <= 3) {
        if (!joinFight(a)) break;                  // alle Plaetze belegt: warten
        break;
      }
      stepTowards(a, beetleX, beetleY);
      if (pixelDist(a.x, a.y, beetleX, beetleY) <= 2) joinFight(a);
      break;

    case ANT_FIGHT:
      updateFighter(a);
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
  // Neuer Kaefer, Heilung
  if (!beetleActive && (int32_t)(now - nextBeetleMs) >= 0) spawnBeetle();
  if (beetleActive && now - lastRegenMs >= REGEN_MS) {
    lastRegenMs += REGEN_MS;
    if (beetleHp < BEETLE_HP) beetleHp++;
  }

  moveSugar();
  moveBeetle();
  // Kaefer frisst Zucker: volle Lebensenergie
  // (Abstand 3: bevor der Kaefer die Traeger am Zucker beruehrt)
  if (beetleActive && sugarActive && pixelDist(beetleX, beetleY, sugarX, sugarY) <= 3) {
    beetleHp = BEETLE_HP;
    removeSugar(false);
  }
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

  // Kaefer an der alten Stelle loeschen
  int16_t bpx = beetleX >> 8, bpy = beetleY >> 8;
  if (beetlePx >= 0 && (!beetleActive || bpx != beetlePx || bpy != beetlePy)) {
    for (int8_t oy = -1; oy <= 1; oy++) {
      for (int8_t ox = -1; ox <= 1; ox++) {
        tft.drawPixel(beetlePx + ox, beetlePy + oy, groundColor(beetlePx + ox, beetlePy + oy));
      }
    }
    beetlePx = -1;
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
    removeSugar(true);
  }

  // Zucker und Ameisen zeichnen
  if (sugarActive) {
    tft.fillRect(spx - 1, spy - 1, 3, 3, COLOR_SUGAR);
    sugarPx = spx;
    sugarPy = spy;
  }
  if (beetleActive) {
    tft.fillRect(bpx - 1, bpy - 1, 3, 3, COLOR_BEETLE);
    beetlePx = bpx;
    beetlePy = bpy;
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
