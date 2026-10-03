/*
  Classic Real Time Strategie Simulator

  Hardware:
    - Arduino UNO R3
    - 1.8" TFT-Shield (ST7735, 160x128)

  Ablauf:
    - Zwei Parteien (BLAU links, ROT rechts) mit je 2 Panzern, 1 Artillerie und
      5 Soldaten kaempfen automatisch gegeneinander. Jede Einheit ist hoechstens
      8x8 Pixel gross (7x7 Symbol + 1 Pixel Lebensbalken).
    - Hindernisse werden per Zufall gesetzt. Panzer und Soldaten brauchen freie
      Sicht auf das Ziel, die Artillerie schiesst ueber Hindernisse hinweg.
    - Hat eine Partei verloren, wird 10 s lang angezeigt, wie lange jede Einheit
      im Spiel war und ob sie ausgeschieden ist. Danach beginnt eine neue
      Schlacht mit neuen Zufallspositionen.

  Trefferregeln (Treffer bis zum Ausscheiden):
                     Ziel: Soldat   Panzer   Artillerie
    Soldat                  10      -        10
    Panzer                   1      4         2
    Artillerie               1      4         2

  Bibliotheken:
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ---------------------------------------------------------------------------
// Pins und Display (wie beim Picture Viewer: TFT-Shield)
// ---------------------------------------------------------------------------
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

#define TFT_TAB       INITR_BLACKTAB
#define TFT_ROTATION  1               // Querformat 160x128

const int16_t SCREEN_W = 160;
const int16_t SCREEN_H = 128;
const int16_t HUD_H    = 10;          // Statuszeile oben
const int16_t FIELD_Y0 = HUD_H;

// ---------------------------------------------------------------------------
// Spielregeln
// ---------------------------------------------------------------------------
enum UnitType : uint8_t { SOLDIER, TANK, ARTILLERY };

const uint8_t  TEAM_SIZE   = 8;              // 2 Panzer, 1 Artillerie, 5 Soldaten
const uint8_t  UNIT_COUNT  = 2 * TEAM_SIZE;
const uint8_t  MAX_HP      = 20;             // gemeinsamer Massstab fuer alle Einheiten
const uint16_t TICK_MS     = 40;             // 25 Simulationsschritte pro Sekunde
const uint32_t RESULT_MS   = 10000UL;        // Ergebnisanzeige
const uint32_t MAX_BATTLE_MS = 180000UL;     // Abbruch nach 3 Minuten (Patt)

// Werte je Einheitentyp:          Soldat  Panzer  Artillerie
const uint8_t RANGE[3]          = { 10,     30,     50 };   // Reichweite in Pixel
const uint8_t RELOAD_TICKS[3]   = { 12,     40,     75 };   // Nachladezeit
const uint8_t MOVE_TICKS[3]     = { 3,      2,      5  };   // Schritte je Pixel
const uint8_t HIT_CHANCE[3]     = { 50,     60,     40 };   // Trefferchance in %

// Schaden je Treffer [Angreifer][Ziel], MAX_HP / Treffer bis zum Ausscheiden
const uint8_t DAMAGE[3][3] = {
  //  Soldat       Panzer       Artillerie
  { MAX_HP / 10,  0,           MAX_HP / 10 },   // Soldat
  { MAX_HP,       MAX_HP / 4,  MAX_HP / 2  },   // Panzer
  { MAX_HP,       MAX_HP / 4,  MAX_HP / 2  }    // Artillerie
};

// Aufstellung einer Partei
const UnitType TEAM_SETUP[TEAM_SIZE] = {
  TANK, TANK, ARTILLERY, SOLDIER, SOLDIER, SOLDIER, SOLDIER, SOLDIER
};
const char UNIT_NAMES[TEAM_SIZE][4] = {
  "Pz1", "Pz2", "Art", "So1", "So2", "So3", "So4", "So5"
};

// ---------------------------------------------------------------------------
// Farben und Symbole
// ---------------------------------------------------------------------------
const uint16_t TEAM_COLOR[2] = { 0x3CDF /* blau */, ST77XX_RED };
const char     TEAM_NAME[2][5] = { "BLAU", "ROT" };
const uint16_t ROCK_COLOR[2] = { 0x7BEF /* grau */, 0x0320 /* dunkelgruen */ };
const uint16_t TRACER_COLOR  = ST77XX_YELLOW;
const uint16_t BLAST_COLOR   = 0xFD20;       // orange

// 7x7-Symbole, Bit 6 = linke Spalte, Blickrichtung nach rechts
const uint8_t SPRITES[3][7] PROGMEM = {
  { 0x1C, 0x1C, 0x3E, 0x1C, 0x1C, 0x14, 0x36 },   // Soldat
  { 0x1C, 0x1F, 0x7F, 0x7F, 0x7F, 0x55, 0x2A },   // Panzer mit Rohr
  { 0x03, 0x06, 0x0C, 0x3E, 0x7F, 0x22, 0x22 }    // Artillerie
};

// ---------------------------------------------------------------------------
// Zustaende
// ---------------------------------------------------------------------------
struct Unit {
  uint8_t  x, y;           // Mittelpunkt (Symbol von x-3..x+3, y-3..y+4)
  uint8_t  oldX, oldY;
  UnitType type;
  uint8_t  team;
  uint8_t  hp;
  uint8_t  cooldown;
  uint8_t  moveTimer;
  int8_t   detourDx, detourDy;
  uint8_t  detourTicks;
  bool     facingLeft;
  bool     dirty;          // neu zeichnen
  uint16_t lifeTenths;     // Zeit im Spiel in 0,1 s (bis zum Ausscheiden)
};

struct Rock {
  uint8_t x, y, w, h;
  uint8_t color;
};

struct Effect {
  uint8_t x0, y0, x1, y1;
  uint8_t kind;            // 0 = frei, 1 = Leuchtspur, 2 = Einschlag
  uint8_t ttl;
};

const uint8_t MAX_ROCKS   = 10;
const uint8_t MAX_EFFECTS = 12;

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

Unit     units[UNIT_COUNT];
Rock     rocks[MAX_ROCKS];
uint8_t  rockCount = 0;
Effect   effects[MAX_EFFECTS];
uint32_t battleStartMs = 0;
uint32_t lastTickMs = 0;
uint16_t lastHudSecond = 0xFFFF;

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
static inline int8_t sign(int16_t v) {
  return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

int32_t dist2(const Unit &a, const Unit &b) {
  int16_t dx = (int16_t)a.x - b.x;
  int16_t dy = (int16_t)a.y - b.y;
  return (int32_t)dx * dx + (int32_t)dy * dy;
}

uint16_t battleTenths() {
  return (millis() - battleStartMs) / 100;
}

bool pointInRock(int16_t px, int16_t py) {
  for (uint8_t i = 0; i < rockCount; i++) {
    const Rock &r = rocks[i];
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return true;
  }
  return false;
}

// Box einer Einheit (8x8) ueberschneidet ein Rechteck?
bool boxHits(int16_t ux, int16_t uy, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return ux - 3 < rx + rw && ux + 5 > rx && uy - 3 < ry + rh && uy + 5 > ry;
}

// Darf die Einheit idx auf (nx, ny) stehen?
bool positionFree(int8_t idx, int16_t nx, int16_t ny) {
  if (nx < 3 || nx > SCREEN_W - 5 || ny < FIELD_Y0 + 3 || ny > SCREEN_H - 5) return false;
  for (uint8_t i = 0; i < rockCount; i++) {
    if (boxHits(nx, ny, rocks[i].x - 1, rocks[i].y - 1, rocks[i].w + 2, rocks[i].h + 2)) return false;
  }
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    if (i == idx || units[i].hp == 0) continue;
    if (abs(nx - units[i].x) < 9 && abs(ny - units[i].y) < 9) return false;
  }
  return true;
}

// Freie Sicht zwischen zwei Einheiten (Hindernisse blockieren)
bool lineOfSight(const Unit &a, const Unit &b) {
  int16_t dx = (int16_t)b.x - a.x;
  int16_t dy = (int16_t)b.y - a.y;
  int16_t steps = max(abs(dx), abs(dy)) / 2;
  for (int16_t s = 1; s < steps; s++) {
    if (pointInRock(a.x + dx * s / steps, a.y + dy * s / steps)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Zeichnen
// ---------------------------------------------------------------------------
// Zeichnet die ganze 8x8-Box einer Einheit in einem Rutsch: Symbol in
// Parteifarbe, darunter Lebensbalken, Rest schwarz
void drawUnit(Unit &u) {
  uint16_t pixels[64];
  uint16_t color = TEAM_COLOR[u.team];
  for (uint8_t row = 0; row < 7; row++) {
    uint8_t bits = pgm_read_byte(&SPRITES[u.type][row]);
    for (uint8_t col = 0; col < 8; col++) {
      bool on = false;
      if (col < 7) {
        uint8_t c = u.facingLeft ? col : 6 - col;
        on = bits & (1 << c);
      }
      pixels[row * 8 + col] = on ? color : ST77XX_BLACK;
    }
  }
  uint8_t bar = (uint16_t)u.hp * 7 / MAX_HP;
  if (bar == 0 && u.hp > 0) bar = 1;
  uint16_t barColor = u.hp > MAX_HP / 2 ? ST77XX_GREEN : (u.hp > MAX_HP / 4 ? ST77XX_YELLOW : ST77XX_RED);
  for (uint8_t col = 0; col < 8; col++) {
    pixels[56 + col] = col < bar ? barColor : ST77XX_BLACK;
  }
  tft.startWrite();
  tft.setAddrWindow(u.x - 3, u.y - 3, 8, 8);
  tft.writePixels(pixels, 64);
  tft.endWrite();
  u.dirty = false;
}

void eraseBox(uint8_t x, uint8_t y) {
  tft.fillRect(x - 3, y - 3, 8, 8, ST77XX_BLACK);
}

void drawRocks() {
  for (uint8_t i = 0; i < rockCount; i++) {
    const Rock &r = rocks[i];
    tft.fillRect(r.x, r.y, r.w, r.h, ROCK_COLOR[r.color]);
  }
}

void drawEffect(const Effect &e, bool erase) {
  if (e.kind == 1) {
    tft.drawLine(e.x0, e.y0, e.x1, e.y1, erase ? ST77XX_BLACK : TRACER_COLOR);
  } else if (e.kind == 2) {
    tft.fillCircle(e.x1, e.y1, 3, erase ? ST77XX_BLACK : BLAST_COLOR);
  }
}

void addEffect(uint8_t kind, const Unit &from, const Unit &to) {
  for (uint8_t i = 0; i < MAX_EFFECTS; i++) {
    if (effects[i].kind == 0) {
      effects[i] = { from.x, from.y, to.x, to.y, kind, 3 };
      drawEffect(effects[i], false);
      return;
    }
  }
}

uint8_t aliveCount(uint8_t team) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    if (units[i].team == team && units[i].hp > 0) n++;
  }
  return n;
}

void drawHud() {
  uint16_t seconds = battleTenths() / 10;
  if (seconds == lastHudSecond) return;
  lastHudSecond = seconds;
  tft.setTextSize(1);
  tft.setCursor(2, 1);
  tft.setTextColor(TEAM_COLOR[0], ST77XX_BLACK);
  tft.print(F("BLAU "));
  tft.print(aliveCount(0));
  tft.setCursor(58, 1);
  tft.setTextColor(TEAM_COLOR[1], ST77XX_BLACK);
  tft.print(F("ROT "));
  tft.print(aliveCount(1));
  tft.setCursor(118, 1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.print(seconds);
  tft.print(F("s  "));
}

// ---------------------------------------------------------------------------
// Aufbau einer neuen Schlacht
// ---------------------------------------------------------------------------
void placeRocks() {
  rockCount = random(6, MAX_ROCKS + 1);
  for (uint8_t i = 0; i < rockCount; i++) {
    Rock &r = rocks[i];
    r.w = random(5, 15);
    r.h = random(5, 15);
    // Hindernisse vor allem im Mittelfeld, damit die Aufstellung frei bleibt
    r.x = random(30, SCREEN_W - 30 - r.w);
    r.y = random(FIELD_Y0 + 2, SCREEN_H - 2 - r.h);
    r.color = random(2);
  }
}

void placeUnits() {
  for (uint8_t i = 0; i < UNIT_COUNT; i++) units[i].hp = 0;   // noch nicht auf dem Feld
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    Unit &u = units[i];
    u.team = i / TEAM_SIZE;
    u.type = TEAM_SETUP[i % TEAM_SIZE];
    // BLAU startet links, ROT rechts; Artillerie weiter hinten
    int16_t xMin = u.type == ARTILLERY ? 4 : 6;
    int16_t xMax = u.type == ARTILLERY ? 16 : 40;
    int16_t nx = 0, ny = 0;
    for (uint8_t tries = 0; tries < 200; tries++) {
      nx = random(xMin, xMax);
      ny = random(FIELD_Y0 + 3, SCREEN_H - 5);
      if (u.team == 1) nx = SCREEN_W - 2 - nx;
      if (positionFree(i, nx, ny)) break;
    }
    u.x = u.oldX = nx;
    u.y = u.oldY = ny;
    u.hp = MAX_HP;
    u.cooldown = random(RELOAD_TICKS[u.type]);
    u.moveTimer = 0;
    u.detourTicks = 0;
    u.facingLeft = u.team == 1;
    u.dirty = true;
    u.lifeTenths = 0;
  }
}

void startBattle() {
  randomSeed(analogRead(A0) ^ micros());
  tft.fillScreen(ST77XX_BLACK);
  tft.drawFastHLine(0, HUD_H - 1, SCREEN_W, 0x39E7);
  for (uint8_t i = 0; i < MAX_EFFECTS; i++) effects[i].kind = 0;
  placeRocks();
  placeUnits();
  drawRocks();
  for (uint8_t i = 0; i < UNIT_COUNT; i++) drawUnit(units[i]);
  battleStartMs = millis();
  lastTickMs = battleStartMs;
  lastHudSecond = 0xFFFF;
  drawHud();
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
// Naechster Gegner, den die Einheit beschaedigen kann (-1 = keiner)
int8_t findTarget(const Unit &u, bool &onlyHarmless) {
  int8_t best = -1;
  int32_t bestD = INT32_MAX;
  onlyHarmless = false;
  int8_t nearestAny = -1;
  int32_t nearestAnyD = INT32_MAX;
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    const Unit &t = units[i];
    if (t.hp == 0 || t.team == u.team) continue;
    int32_t d = dist2(u, t);
    if (d < nearestAnyD) { nearestAnyD = d; nearestAny = i; }
    if (DAMAGE[u.type][t.type] == 0) continue;
    if (d < bestD) { bestD = d; best = i; }
  }
  if (best < 0 && nearestAny >= 0) {
    onlyHarmless = true;   // z.B. Soldat gegen Panzer: nur ausweichen
    return nearestAny;
  }
  return best;
}

bool tryMove(uint8_t idx, int16_t nx, int16_t ny) {
  if (!positionFree(idx, nx, ny)) return false;
  Unit &u = units[idx];
  u.x = nx;
  u.y = ny;
  return true;
}

void moveUnit(uint8_t idx, int8_t dx, int8_t dy) {
  Unit &u = units[idx];
  if (u.moveTimer > 0) {
    u.moveTimer--;
    return;
  }
  u.moveTimer = MOVE_TICKS[u.type] - 1;

  if (u.detourTicks > 0) {
    u.detourTicks--;
    if (tryMove(idx, u.x + u.detourDx, u.y + u.detourDy)) return;
    u.detourTicks = 0;
  }
  if (tryMove(idx, u.x + dx, u.y + dy)) return;
  if (dx != 0 && tryMove(idx, u.x + dx, u.y)) return;
  if (dy != 0 && tryMove(idx, u.x, u.y + dy)) return;

  // Blockiert: eine Weile in eine zufaellige Richtung ausweichen
  do {
    u.detourDx = random(-1, 2);
    u.detourDy = random(-1, 2);
  } while (u.detourDx == 0 && u.detourDy == 0);
  u.detourTicks = random(8, 30);
}

void destroyUnit(Unit &t) {
  t.hp = 0;
  t.lifeTenths = battleTenths();
  eraseBox(t.oldX, t.oldY);   // falls sie sich in diesem Schritt bewegt hat
  eraseBox(t.x, t.y);
  lastHudSecond = 0xFFFF;   // Statuszeile aktualisieren
}

void updateUnit(uint8_t idx) {
  Unit &u = units[idx];
  if (u.cooldown > 0) u.cooldown--;

  bool onlyHarmless;
  int8_t ti = findTarget(u, onlyHarmless);
  if (ti < 0) return;
  Unit &t = units[ti];
  int8_t dx = sign((int16_t)t.x - u.x);
  int8_t dy = sign((int16_t)t.y - u.y);

  if (onlyHarmless) {
    moveUnit(idx, -dx, -dy);   // Abstand halten
    return;
  }

  bool facing = dx < 0;
  if (dx != 0 && facing != u.facingLeft) {
    u.facingLeft = facing;
    u.dirty = true;
  }

  int16_t range = RANGE[u.type];
  bool inRange = dist2(u, t) <= (int32_t)range * range &&
                 (u.type == ARTILLERY || lineOfSight(u, t));
  if (!inRange) {
    moveUnit(idx, dx, dy);
    return;
  }
  if (u.cooldown > 0) return;

  // Schuss
  u.cooldown = RELOAD_TICKS[u.type];
  addEffect(u.type == ARTILLERY ? 2 : 1, u, t);
  if ((uint8_t)random(100) >= HIT_CHANCE[u.type]) return;   // daneben
  uint8_t damage = DAMAGE[u.type][t.type];
  if (damage >= t.hp) destroyUnit(t);
  else {
    t.hp -= damage;
    t.dirty = true;
  }
}

// Ein Simulationsschritt. Liefert true, solange beide Parteien leben.
bool tick() {
  // Effekte altern lassen, abgelaufene loeschen
  bool erased = false;
  for (uint8_t i = 0; i < MAX_EFFECTS; i++) {
    Effect &e = effects[i];
    if (e.kind == 0) continue;
    if (--e.ttl == 0) {
      drawEffect(e, true);
      e.kind = 0;
      erased = true;
    }
  }

  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    if (units[i].hp > 0) updateUnit(i);
  }

  // Bewegte Einheiten an der alten Stelle loeschen
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    Unit &u = units[i];
    if (u.hp == 0) continue;
    if (u.x != u.oldX || u.y != u.oldY) {
      eraseBox(u.oldX, u.oldY);
      u.oldX = u.x;
      u.oldY = u.y;
      u.dirty = true;
    }
  }
  // Geloeschte Effekte koennen Hindernisse und Einheiten ueberdeckt haben
  if (erased) drawRocks();
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    Unit &u = units[i];
    if (u.hp > 0 && (u.dirty || erased)) drawUnit(u);
  }
  // Noch sichtbare Effekte wieder obenauf
  if (erased) {
    for (uint8_t i = 0; i < MAX_EFFECTS; i++) {
      if (effects[i].kind != 0) drawEffect(effects[i], false);
    }
  }
  drawHud();

  return aliveCount(0) > 0 && aliveCount(1) > 0 && millis() - battleStartMs < MAX_BATTLE_MS;
}

// ---------------------------------------------------------------------------
// Ergebnis
// ---------------------------------------------------------------------------
void printTenths(uint16_t tenths) {
  tft.print(' ');
  if (tenths < 1000) tft.print(' ');
  if (tenths < 100) tft.print(' ');
  tft.print(tenths / 10);
  tft.print('.');
  tft.print(tenths % 10);
  tft.print('s');
}

uint16_t remainingHp(uint8_t team) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    if (units[i].team == team) sum += units[i].hp;
  }
  return sum;
}

void showResult() {
  uint16_t total = battleTenths();
  // Ueberlebende waren bis zum Schluss im Spiel
  for (uint8_t i = 0; i < UNIT_COUNT; i++) {
    if (units[i].hp > 0) units[i].lifeTenths = total;
  }

  // Sieger: wer noch Einheiten hat; bei Zeitablauf die meisten Lebenspunkte
  int8_t winner = -1;
  uint8_t alive0 = aliveCount(0), alive1 = aliveCount(1);
  if (alive0 > 0 && alive1 == 0) winner = 0;
  else if (alive1 > 0 && alive0 == 0) winner = 1;
  else if (alive0 > 0 && alive1 > 0) {
    uint16_t hp0 = remainingHp(0), hp1 = remainingHp(1);
    if (hp0 != hp1) winner = hp0 > hp1 ? 0 : 1;
  }

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(4, 2);
  if (winner < 0) {
    tft.setTextColor(ST77XX_WHITE);
    tft.print(F("UNENTSCHIEDEN"));
  } else {
    tft.setTextColor(TEAM_COLOR[winner]);
    tft.print(TEAM_NAME[winner]);
    tft.print(F(" SIEGT"));
  }

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 22);
  tft.print(F("Dauer:"));
  printTenths(total);
  if (alive0 > 0 && alive1 > 0) tft.print(F(" Zeitende"));

  for (uint8_t team = 0; team < 2; team++) {
    int16_t x = 2 + team * 80;
    tft.setTextColor(TEAM_COLOR[team]);
    tft.setCursor(x, 36);
    tft.print(TEAM_NAME[team]);
    for (uint8_t k = 0; k < TEAM_SIZE; k++) {
      const Unit &u = units[team * TEAM_SIZE + k];
      tft.setCursor(x, 48 + k * 10);
      tft.setTextColor(u.hp > 0 ? ST77XX_WHITE : 0x7BEF);
      tft.print(UNIT_NAMES[k]);
      printTenths(u.lifeTenths);
      tft.print(u.hp > 0 ? F(" ok") : F(" x"));
    }
  }
  delay(RESULT_MS);
}

// ---------------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------------
void setup() {
  tft.initR(TFT_TAB);
  tft.setRotation(TFT_ROTATION);
  tft.setTextWrap(false);
  startBattle();
}

void loop() {
  if (millis() - lastTickMs < TICK_MS) return;
  lastTickMs += TICK_MS;
  if (!tick()) {
    showResult();
    startBattle();
  }
}
