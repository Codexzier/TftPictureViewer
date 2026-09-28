/*
  Picture Viewer with Sense

  Hardware:
    - Arduino UNO R3
    - 1.8" TFT (ST7735, 160x128) mit SD-Kartenleser (SPI)
    - MPU6050 (Beschleunigung + Gyroskop) an I2C (SDA=A4, SCL=A5)

  Ablauf:
    1. Nach dem Einschalten wird die Lage des Sensors eingelesen und als neues
       Offset gespeichert. Dabei werden die 6 Achsenwerte auf dem TFT angezeigt.
    2. Der Titel "Picture Viewer with Sense" wird angezeigt.
    3. Diashow: Alle 30 Sekunden wird zufaellig ein Bild von der SD-Karte
       geladen. Links bleibt ein 20 Pixel breiter schwarzer Streifen, auf dem
       die 6 Achsen als vertikale Balken in 6 Farben dargestellt werden.
    4. Wird der Arduino angehoben, verschwindet das Bild und das Diagramm wird
       horizontal ueber den ganzen Bildschirm angezeigt. Liegt er wieder ruhig,
       wird die Diashow fortgesetzt.

  Bilder:
    Der UNO hat nur 2 KB RAM - zu wenig fuer einen JPEG-Decoder zusaetzlich zur
    SD-Bibliothek. Die JPEG-Bilder werden deshalb vorher mit
    tools/convert_images.py in 24-Bit-BMP (140x128) umgewandelt und als *.BMP
    ins Hauptverzeichnis der SD-Karte kopiert.

  Bibliotheken:
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - SD (Arduino)
*/

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ---------------------------------------------------------------------------
// Pins und Display
// ---------------------------------------------------------------------------
#define TFT_CS   10
#define TFT_DC    8
#define TFT_RST   9
#define SD_CS     4

// Je nach Display-Variante INITR_BLACKTAB, INITR_GREENTAB oder INITR_REDTAB
#define TFT_TAB       INITR_BLACKTAB
#define TFT_ROTATION  1               // Querformat 160x128

const int16_t SCREEN_W = 160;
const int16_t SCREEN_H = 128;

// Linker Streifen fuer das vertikale Diagramm, rechts daneben das Bild
const int16_t STRIP_W = 20;
const int16_t IMG_X   = STRIP_W;
const int16_t IMG_W   = SCREEN_W - STRIP_W;   // 140
const int16_t IMG_H   = SCREEN_H;             // 128

// Vertikale Balken im Streifen: 6 Balken, je 2 px breit, 1 px Abstand
const int16_t V_BAR_X0    = 1;
const int16_t V_BAR_PITCH = 3;
const int16_t V_BAR_W     = 2;
const int16_t V_CENTER_Y  = SCREEN_H / 2;
const uint8_t V_HALF      = SCREEN_H / 2;     // 64 px je Richtung

// Horizontales Vollbild-Diagramm: 6 Zeilen mit Beschriftung links
const int16_t H_LABEL_W   = 20;
const int16_t H_ROW_Y0    = 1;
const int16_t H_ROW_H     = 21;
const int16_t H_BAR_OFS   = 3;
const int16_t H_BAR_H     = 15;
const uint8_t H_HALF      = (SCREEN_W - H_LABEL_W) / 2;   // 70 px je Richtung
const int16_t H_CENTER_X  = H_LABEL_W + H_HALF;

// Farben der 6 Achsen: AX, AY, AZ, GX, GY, GZ
const uint16_t CH_COLORS[6] = {
  ST77XX_RED, ST77XX_GREEN, 0x3CDF /* hellblau */,
  ST77XX_YELLOW, ST77XX_CYAN, ST77XX_MAGENTA
};
const char CH_LABELS[6][3] = { "AX", "AY", "AZ", "GX", "GY", "GZ" };

// ---------------------------------------------------------------------------
// Zeiten
// ---------------------------------------------------------------------------
const uint32_t SLIDE_INTERVAL_MS  = 30000UL;  // Bildwechsel
const uint8_t  SAMPLE_INTERVAL_MS = 10;       // 100 Hz Sensorabtastung
const uint16_t CALIB_SAMPLES      = 200;      // 2 s Offset-Messung
const uint16_t NOISE_SAMPLES      = 100;      // 1 s Ruherauschen messen
const uint16_t TITLE_TIME_MS      = 3000;
const uint16_t REST_TIME_MS       = 1500;     // so lange ruhig -> Diashow

// ---------------------------------------------------------------------------
// MPU6050
// ---------------------------------------------------------------------------
const uint8_t MPU_ADDR          = 0x68;
const uint8_t MPU_SMPLRT_DIV    = 0x19;
const uint8_t MPU_CONFIG        = 0x1A;
const uint8_t MPU_GYRO_CONFIG   = 0x1B;
const uint8_t MPU_ACCEL_CONFIG  = 0x1C;
const uint8_t MPU_ACCEL_XOUT_H  = 0x3B;
const uint8_t MPU_PWR_MGMT_1    = 0x6B;

// Empfindlichste Einstellungen, damit kleinste Vibrationen und
// Drehgeschwindigkeiten sichtbar werden:
//   Beschleunigung +-2 g     -> 16384 LSB/g
//   Gyroskop      +-250 dps  -> 131 LSB/(grad/s)
//   DLPF 2 (~94 Hz)          -> hohe Bandbreite fuer Vibrationen
//   Abtastrate 1 kHz / (1+9) = 100 Hz
const uint8_t MPU_DLPF_CFG      = 0x02;
const uint8_t MPU_GYRO_FS_250   = 0x00;
const uint8_t MPU_ACCEL_FS_2G   = 0x00;

// Balkenskalierung: Wurzelkennlinie, damit kleine Werte gut sichtbar sind und
// grosse Werte trotzdem nicht sofort am Rand kleben. Vollausschlag bei
// FULL_SCALE_SQRT^2 = 4096 LSB (0,25 g bzw. ~31 grad/s).
const uint8_t FULL_SCALE_SQRT = 64;

// Bewegungserkennung (Schwellen werden aus dem Ruherauschen berechnet)
const int32_t  LIFT_MARGIN     = 60;
const int32_t  REST_MARGIN     = 30;
const int32_t  NOISE_MAX       = 800;   // Begrenzung, falls beim Start bewegt
const int32_t  TILT_THRESHOLD  = 2500;  // Summe |dA| ~0,15 g -> Lage geaendert
const uint8_t  LIFT_CONFIRM    = 3;     // Abtastungen in Folge

const uint8_t  CHUNK_PIXELS    = 16;    // BMP-Lesepuffer

// ---------------------------------------------------------------------------
// Globale Zustaende
// ---------------------------------------------------------------------------
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

enum Mode : uint8_t { MODE_SLIDESHOW, MODE_LIFTED };
Mode mode = MODE_SLIDESHOW;

bool     sensorOk = false;
uint8_t  sensorErrors = 0;
int16_t  raw[6];          // AX AY AZ GX GY GZ
int16_t  offset[6];
int16_t  prevAcc[3];
int32_t  activity = 0;    // gefilterte Bewegungsstaerke
int32_t  liftThreshold = 400;
int32_t  restThreshold = 200;
uint8_t  liftCount = 0;
uint32_t lastSampleMs = 0;

uint32_t restSinceMs = 0;
uint16_t restCount = 0;
int32_t  restSum[3];

int8_t   barLen[6];       // aktuell gezeichnete Balkenlaenge je Achse

bool     sdOk = false;
uint16_t imageCount = 0;
int16_t  lastImageIndex = -1;
bool     imageRequested = false;
uint32_t lastSlideMs = 0;

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------
static inline uint32_t absDiff(int32_t a, int32_t b) {
  int32_t d = a - b;
  return d < 0 ? -d : d;
}

uint16_t isqrt(uint32_t n) {
  uint32_t res = 0;
  uint32_t bit = 1UL << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

// Zahl rechtsbuendig mit fester Breite ausgeben (ueberschreibt alte Werte)
void printValue(int32_t v, uint8_t width) {
  char buf[12];
  ltoa(v, buf, 10);
  for (uint8_t n = strlen(buf); n < width; n++) tft.print(' ');
  tft.print(buf);
}

void printCentered(const __FlashStringHelper *text, int16_t y, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((SCREEN_W - (int16_t)w) / 2, y);
  tft.setTextColor(color);
  tft.print(text);
}

// ---------------------------------------------------------------------------
// MPU6050
// ---------------------------------------------------------------------------
bool mpuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool mpuInit() {
  Wire.begin();
  Wire.setClock(400000);
  Wire.setWireTimeout(3000, true);
  delay(100);
  return mpuWrite(MPU_PWR_MGMT_1, 0x01)            // aufwecken, PLL Gyro X
      && mpuWrite(MPU_SMPLRT_DIV, 9)                // 100 Hz
      && mpuWrite(MPU_CONFIG, MPU_DLPF_CFG)
      && mpuWrite(MPU_GYRO_CONFIG, MPU_GYRO_FS_250)
      && mpuWrite(MPU_ACCEL_CONFIG, MPU_ACCEL_FS_2G);
}

bool mpuRead(int16_t *values) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(MPU_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14) != 14) return false;
  for (uint8_t i = 0; i < 7; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    int16_t v = (int16_t)((hi << 8) | lo);
    if (i < 3) values[i] = v;            // Beschleunigung
    else if (i > 3) values[i - 1] = v;   // Gyro (i == 3 ist Temperatur)
  }
  return true;
}

// Liest einen neuen Messwert, sobald das Abtastintervall abgelaufen ist.
// Liefert true, wenn ein neuer Wert vorliegt.
bool sampleSensor() {
  if (!sensorOk) return false;
  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return false;
  lastSampleMs = now;
  if (!mpuRead(raw)) {
    if (++sensorErrors >= 20) sensorOk = false;   // Sensor antwortet nicht mehr
    return false;
  }
  sensorErrors = 0;

  // Bewegungsstaerke: Aenderung der Beschleunigung (Ruck) + Drehrate
  uint32_t act = 0;
  for (uint8_t i = 0; i < 3; i++) {
    act += absDiff(raw[i], prevAcc[i]);
    prevAcc[i] = raw[i];
  }
  for (uint8_t i = 3; i < 6; i++) {
    act += absDiff(raw[i], offset[i]);
  }
  activity += ((int32_t)act - activity) / 8;
  return true;
}

// Lageaenderung gegenueber dem Offset (Summe der Beschleunigungsabweichungen)
uint32_t tilt() {
  uint32_t t = 0;
  for (uint8_t i = 0; i < 3; i++) t += absDiff(raw[i], offset[i]);
  return t;
}

void waitWithSensor(uint16_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) sampleSensor();
}

// ---------------------------------------------------------------------------
// Diagramm
// ---------------------------------------------------------------------------
int8_t scaleValue(int32_t d, uint8_t half) {
  uint32_t a = d < 0 ? -d : d;
  uint16_t px = (uint32_t)isqrt(a) * half / FULL_SCALE_SQRT;
  if (px > half) px = half;
  return d < 0 ? -(int8_t)px : (int8_t)px;
}

// Fuellt den Bereich [lo, hi) relativ zur Nulllinie eines Balkens.
// Vertikal: positiv nach oben. Horizontal: positiv nach rechts.
void fillBarRange(uint8_t ch, int16_t lo, int16_t hi, uint16_t color, bool horizontal) {
  if (hi <= lo) return;
  if (horizontal) {
    int16_t y = H_ROW_Y0 + ch * H_ROW_H + H_BAR_OFS;
    tft.fillRect(H_CENTER_X + lo, y, hi - lo, H_BAR_H, color);
  } else {
    int16_t x = V_BAR_X0 + ch * V_BAR_PITCH;
    tft.fillRect(x, V_CENTER_Y - hi, V_BAR_W, hi - lo, color);
  }
}

// Zeichnet nur die Differenz zum vorherigen Balken -> kein Flackern
void updateBar(uint8_t ch, int8_t len, bool horizontal) {
  int8_t old = barLen[ch];
  if (old == len) return;
  uint16_t color = CH_COLORS[ch];
  if (old != 0 && len != 0 && (old > 0) == (len > 0)) {
    int16_t lo = min(old, len);
    int16_t hi = max(old, len);
    bool grows = abs(len) > abs(old);
    fillBarRange(ch, lo, hi, grows ? color : ST77XX_BLACK, horizontal);
  } else {
    fillBarRange(ch, min(old, 0), max(old, 0), ST77XX_BLACK, horizontal);
    fillBarRange(ch, min(len, 0), max(len, 0), color, horizontal);
  }
  barLen[ch] = len;
}

void updateBars(bool horizontal) {
  uint8_t half = horizontal ? H_HALF : V_HALF;
  for (uint8_t ch = 0; ch < 6; ch++) {
    updateBar(ch, scaleValue((int32_t)raw[ch] - offset[ch], half), horizontal);
  }
}

void resetBars() {
  for (uint8_t ch = 0; ch < 6; ch++) barLen[ch] = 0;
}

void drawLiftedLayout() {
  tft.setTextSize(1);
  for (uint8_t ch = 0; ch < 6; ch++) {
    int16_t y = H_ROW_Y0 + ch * H_ROW_H;
    tft.setTextColor(CH_COLORS[ch]);
    tft.setCursor(2, y + 7);
    tft.print(CH_LABELS[ch]);
  }
}

// ---------------------------------------------------------------------------
// Moduswechsel
// ---------------------------------------------------------------------------
void enterLifted() {
  mode = MODE_LIFTED;
  tft.fillScreen(ST77XX_BLACK);
  resetBars();
  drawLiftedLayout();
  restCount = 0;
}

void enterSlideshow() {
  mode = MODE_SLIDESHOW;
  tft.fillScreen(ST77XX_BLACK);
  resetBars();
  liftCount = 0;
  imageRequested = true;
}

// Verarbeitet einen neuen Messwert: Diagramm aktualisieren und pruefen, ob
// der Arduino angehoben bzw. wieder abgelegt wurde.
void processSample() {
  if (mode == MODE_SLIDESHOW) {
    updateBars(false);
    if (activity > liftThreshold || tilt() > TILT_THRESHOLD) {
      if (++liftCount >= LIFT_CONFIRM) enterLifted();
    } else {
      liftCount = 0;
    }
    return;
  }

  updateBars(true);
  if (activity < restThreshold) {
    if (restCount == 0) {
      restSinceMs = millis();
      restSum[0] = restSum[1] = restSum[2] = 0;
    }
    for (uint8_t i = 0; i < 3; i++) restSum[i] += raw[i];
    restCount++;
    if (millis() - restSinceMs >= REST_TIME_MS) {
      // Neue Ruhelage als Offset uebernehmen (falls anders abgelegt)
      for (uint8_t i = 0; i < 3; i++) offset[i] = restSum[i] / (int32_t)restCount;
      enterSlideshow();
    }
  } else {
    restCount = 0;
  }
}

void serviceSensor() {
  if (sampleSensor()) processSample();
}

// ---------------------------------------------------------------------------
// Kalibrierung
// ---------------------------------------------------------------------------
void drawAxisValues(const int16_t *values, const int16_t *ref, int16_t y0) {
  tft.setTextSize(1);
  for (uint8_t ch = 0; ch < 6; ch++) {
    tft.setTextColor(CH_COLORS[ch], ST77XX_BLACK);
    tft.setCursor(10 + (ch / 3) * 75, y0 + (ch % 3) * 12);
    tft.print(CH_LABELS[ch]);
    tft.print(':');
    printValue(ref ? (int32_t)values[ch] - ref[ch] : values[ch], 7);
  }
}

void calibrate() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 4);
  tft.print(F("Sensor-Kalibrierung"));
  tft.setCursor(4, 16);
  tft.print(F("Bitte nicht bewegen..."));

  if (!sensorOk) {
    tft.setTextColor(ST77XX_RED);
    tft.setCursor(4, 40);
    tft.print(F("MPU6050 nicht gefunden!"));
    delay(3000);
    return;
  }

  tft.setCursor(4, 34);
  tft.print(F("Lage (roh):"));

  // Phase 1: Mittelwert der Ruhelage als Offset
  int32_t sum[6] = { 0 };
  uint16_t n = 0;
  uint16_t seed = analogRead(A0);
  while (n < CALIB_SAMPLES && sensorOk) {
    if (!sampleSensor()) continue;
    for (uint8_t ch = 0; ch < 6; ch++) sum[ch] += raw[ch];
    seed = (seed << 1) ^ raw[n % 6];
    n++;
    if (n % 10 == 0) drawAxisValues(raw, NULL, 46);
  }
  for (uint8_t ch = 0; ch < 6; ch++) offset[ch] = sum[ch] / (int32_t)CALIB_SAMPLES;
  randomSeed(seed);

  drawAxisValues(offset, NULL, 46);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(4, 34);
  tft.print(F("Offset:     "));

  // Phase 2: Ruherauschen messen -> Schwellen fuer die Bewegungserkennung
  tft.setCursor(4, 88);
  tft.print(F("Abweichung zum Offset:"));
  activity = 0;
  int32_t noise = 0;
  n = 0;
  while (n < NOISE_SAMPLES && sensorOk) {
    if (!sampleSensor()) continue;
    if (n > 10 && activity > noise) noise = activity;   // Filter einschwingen lassen
    n++;
    if (n % 10 == 0) drawAxisValues(raw, offset, 100);
  }
  if (noise > NOISE_MAX) noise = NOISE_MAX;
  liftThreshold = noise * 2 + LIFT_MARGIN;
  restThreshold = noise + noise / 2 + REST_MARGIN;

  delay(1500);
}

// ---------------------------------------------------------------------------
// SD-Karte und Bilder
// ---------------------------------------------------------------------------
bool isImageFile(File &entry) {
  if (entry.isDirectory()) return false;
  const char *name = entry.name();
  if (name[0] == '_' || name[0] == '.') return false;   // z.B. macOS "._*"
  const char *dot = strrchr(name, '.');
  return dot && strcasecmp(dot, ".BMP") == 0;
}

uint16_t countImages() {
  File root = SD.open("/");
  if (!root) return 0;
  uint16_t count = 0;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (isImageFile(entry)) count++;
    entry.close();
  }
  root.close();
  return count;
}

bool imageNameAt(uint16_t index, char *out) {
  File root = SD.open("/");
  if (!root) return false;
  bool found = false;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (isImageFile(entry)) {
      if (index == 0) {
        strncpy(out, entry.name(), 12);
        out[12] = '\0';
        found = true;
      }
      index--;
    }
    entry.close();
    if (found) break;
  }
  root.close();
  return found;
}

uint16_t read16(File &f) {
  uint16_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  return r;
}

uint32_t read32(File &f) {
  uint32_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  ((uint8_t *)&r)[2] = f.read();
  ((uint8_t *)&r)[3] = f.read();
  return r;
}

// Laedt ein unkomprimiertes 24-Bit-BMP zeilenweise in den Bildbereich.
// Groessere Bilder werden mittig beschnitten, kleinere zentriert.
// Zwischen den Zeilen wird der Sensor abgefragt; wird der Arduino dabei
// angehoben, bricht das Laden ab.
bool loadBmp(const char *name) {
  File f = SD.open(name);
  if (!f) return false;

  if (read16(f) != 0x4D42) { f.close(); return false; }   // "BM"
  read32(f);                                              // Dateigroesse
  read32(f);                                              // reserviert
  uint32_t dataOffset = read32(f);
  read32(f);                                              // Headergroesse
  int32_t w = (int32_t)read32(f);
  int32_t h = (int32_t)read32(f);
  if (read16(f) != 1 || read16(f) != 24 || read32(f) != 0 || w <= 0 || h == 0) {
    f.close();
    return false;
  }

  bool bottomUp = h > 0;
  if (h < 0) h = -h;
  uint32_t rowSize = ((uint32_t)w * 3 + 3) & ~3UL;

  int16_t dw = w < IMG_W ? w : IMG_W;
  int16_t dh = h < IMG_H ? h : IMG_H;
  int16_t x0 = IMG_X + (IMG_W - dw) / 2;
  int16_t y0 = (IMG_H - dh) / 2;
  uint16_t cropX = (w - dw) / 2;
  uint16_t cropY = (h - dh) / 2;

  if (dw < IMG_W || dh < IMG_H) tft.fillRect(IMG_X, 0, IMG_W, IMG_H, ST77XX_BLACK);

  uint8_t  sdBuf[CHUNK_PIXELS * 3];
  uint16_t pixels[CHUNK_PIXELS];

  // Zeilen in Dateireihenfolge lesen (nur Vorwaerts-Seek auf der SD-Karte)
  for (int16_t i = 0; i < dh; i++) {
    serviceSensor();
    if (mode != MODE_SLIDESHOW) { f.close(); return false; }

    uint16_t imgRow  = bottomUp ? (cropY + dh - 1 - i) : (cropY + i);
    uint32_t fileRow = bottomUp ? (uint32_t)(h - 1 - imgRow) : imgRow;
    uint32_t pos = dataOffset + fileRow * rowSize + (uint32_t)cropX * 3;
    if (f.position() != pos && !f.seek(pos)) { f.close(); return false; }

    int16_t y = y0 + (imgRow - cropY);
    int16_t left = dw;
    bool first = true;
    while (left > 0) {
      uint8_t n = left < CHUNK_PIXELS ? left : CHUNK_PIXELS;
      if (f.read(sdBuf, n * 3) != n * 3) { f.close(); return false; }
      for (uint8_t j = 0; j < n; j++) {
        pixels[j] = tft.color565(sdBuf[j * 3 + 2], sdBuf[j * 3 + 1], sdBuf[j * 3]);
      }
      tft.startWrite();
      if (first) {
        tft.setAddrWindow(x0, y, dw, 1);
        first = false;
      }
      tft.writePixels(pixels, n);
      tft.endWrite();
      left -= n;
    }
  }
  f.close();
  return true;
}

void showMessage(const __FlashStringHelper *line1, const __FlashStringHelper *line2) {
  tft.fillRect(IMG_X, 0, IMG_W, IMG_H, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(IMG_X + 6, 54);
  tft.print(line1);
  tft.setCursor(IMG_X + 6, 66);
  tft.print(line2);
}

void showNextImage() {
  lastSlideMs = millis();
  if (!sdOk) {
    showMessage(F("SD-Karte fehlt"), F("oder Fehler"));
    return;
  }
  if (imageCount == 0) {
    showMessage(F("Keine *.BMP Bilder"), F("auf der SD-Karte"));
    return;
  }

  int16_t index = random(imageCount);
  if (imageCount > 1 && index == lastImageIndex) {
    index = (index + 1 + random(imageCount - 1)) % imageCount;
  }
  lastImageIndex = index;

  char name[13];
  if (!imageNameAt(index, name)) return;
  if (!loadBmp(name) && mode == MODE_SLIDESHOW) {
    showMessage(F("Bild ungueltig:"), F("nur 24-Bit-BMP"));
  }
  lastSlideMs = millis();   // 30 s ab fertig angezeigtem Bild
}

// ---------------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------------
void showTitle() {
  tft.fillScreen(ST77XX_BLACK);
  printCentered(F("Picture"), 26, 2, ST77XX_WHITE);
  printCentered(F("Viewer"), 48, 2, ST77XX_WHITE);
  printCentered(F("with Sense"), 70, 2, ST77XX_CYAN);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 112);
  if (sdOk) {
    tft.print(imageCount);
    tft.print(F(" Bilder gefunden"));
  } else {
    tft.print(F("SD-Karte nicht gefunden"));
  }
  waitWithSensor(TITLE_TIME_MS);
}

void setup() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  tft.initR(TFT_TAB);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  sensorOk = mpuInit();
  calibrate();

  sdOk = SD.begin(SD_CS);
  if (sdOk) imageCount = countImages();

  showTitle();
  enterSlideshow();
}

void loop() {
  serviceSensor();

  if (mode == MODE_SLIDESHOW &&
      (imageRequested || millis() - lastSlideMs >= SLIDE_INTERVAL_MS)) {
    imageRequested = false;
    showNextImage();
  }
}
