# TftPictureViewer

Simple digital picture viewer on tft display – **Picture Viewer with Sense**.

Ein Arduino UNO zeigt zufällig Bilder von der SD-Karte auf einem 1.8" TFT an und
visualisiert dabei die 6 Achsen eines MPU6050 (Beschleunigung + Drehrate).

## Funktionen

1. **Kalibrierung:** Nach dem Einschalten wird die Ruhelage des Sensors gemessen
   und als Offset gespeichert. Die 6 Achsenwerte werden dabei angezeigt.
   Anschließend wird das Ruherauschen gemessen, daraus ergeben sich die
   Schwellen für die Bewegungserkennung.
2. **Titel:** "Picture Viewer with Sense".
3. **Diashow:** Alle 30 Sekunden wird zufällig ein Bild von der SD-Karte angezeigt.
   Links bleibt ein 20 px breiter schwarzer Streifen mit 6 farbigen vertikalen
   Balken (AX rot, AY grün, AZ blau, GX gelb, GY cyan, GZ magenta).
4. **Angehoben:** Das Bild verschwindet, die 6 Balken werden horizontal über den
   ganzen Bildschirm angezeigt. Liegt der Arduino wieder 1,5 s ruhig, wird die
   neue Lage als Offset übernommen und die Diashow fortgesetzt.
5. **Nur Sensoranzeige:** Ist keine SD-Karte lesbar oder liegen keine `*.BMP`-Bilder
   darauf, wird dauerhaft nur das horizontale Sensor-Diagramm angezeigt.
6. **Bild über USB/seriell:** Ein Bild kann vom PC gesendet werden
   (siehe [Bild seriell senden](#bild-seriell-senden)).
7. **Performance-Modus:** Sendet der PC Leistungsdaten (`tools/pc_monitor.py`),
   wird die Diashow auf die halbe Breite reduziert. Daneben zeigen 4 runde
   Analog-Anzeigen CPU, GPU, Netzwerk und Festplatte
   (siehe [Leistungsdaten vom PC](#leistungsdaten-vom-pc)).

Der Sensor ist auf maximale Empfindlichkeit eingestellt (±2 g, ±250 °/s,
Tiefpass ~94 Hz, 100 Hz Abtastung). Die Balken verwenden eine Wurzelkennlinie,
damit auch kleinste Vibrationen sichtbar sind.

## Verdrahtung

| Modul            | Pin        | Arduino UNO |
|------------------|------------|-------------|
| TFT              | CS         | D10         |
| TFT              | A0 / DC    | D9          |
| TFT              | RESET      | D8          |
| TFT + SD         | SDA / MOSI | D11         |
| TFT + SD         | SCK        | D13         |
| SD               | MISO       | D12         |
| SD               | CS         | D4          |
| MPU6050          | SDA        | A4          |
| MPU6050          | SCL        | A5          |

Die Pins und die Display-Variante (`INITR_BLACKTAB`, `INITR_GREENTAB`, ...)
lassen sich oben in `TftPictureViewer/TftPictureViewer.ino` anpassen.

## Bibliotheken

- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library
- SD (Arduino)

## Bilder vorbereiten

Der UNO hat nur 2 KB RAM – zu wenig, um JPEGs neben der SD-Bibliothek direkt zu
dekodieren. Die JPEGs werden deshalb am PC in 24-Bit-BMP (140x128) umgewandelt:

```sh
pip install Pillow
python3 tools/convert_images.py meine_fotos/ sd_karte/
```

Den Inhalt von `sd_karte/` ins Hauptverzeichnis der FAT16/FAT32-formatierten
SD-Karte kopieren.

## Bild seriell senden

Über die USB-Verbindung (serielle Schnittstelle, 250000 Baud) kann ein Bild direkt
angezeigt werden. Übertragen wird nur die Zielauflösung des Bildbereichs
(140x128 Pixel, RGB565 = 35.840 Bytes, geschätzt 2–3 s):

```sh
pip install pyserial Pillow
python3 tools/send_image.py COM3 foto.jpg          # Windows
python3 tools/send_image.py /dev/ttyACM0 foto.jpg  # Linux
```

Das Skript skaliert das Bild bei Bedarf auf 140x128. Da das Öffnen des Ports den UNO
zurücksetzt, wartet es auf die Startmeldung `TPV READY`.

Das empfangene Bild wird sofort angezeigt. Mit SD-Bildern geht die Diashow nach
30 s normal weiter; ohne SD-Bilder bleibt das Bild stehen, bis der Arduino
angehoben wird.

Läuft `pc_monitor.py`, belegt es den Port – Bilder dann dort senden (siehe unten).

## Leistungsdaten vom PC

`tools/pc_monitor.py` misst jede Sekunde die Auslastung und sendet sie an den Arduino:

```sh
pip install pyserial Pillow psutil
python3 tools/pc_monitor.py COM3
python3 tools/pc_monitor.py COM3 --image-dir bilder/ --net-max 250
```

Solange Daten ankommen, sieht das Display so aus:

| Streifen (20 px) | 4 Rundinstrumente (70 px)  | Diashow (70 px)            |
|------------------|----------------------------|----------------------------|
| 6 Messbalken     | CPU, GPU / NET, DISK (2x2) | Bildmitte, 70x128 Pixel    |

- Die Instrumente haben eine 270°-Skala mit Farbzonen (grün bis 60 %, gelb bis
  85 %, rot darüber), einen Zeiger und den Wert in Prozent.
- Das Anheben schaltet in diesem Modus **nicht** auf das Vollbild-Diagramm um,
  die Messbalken im Streifen laufen weiter.
- Die SD-Bilder (140x128) werden auf die mittleren 70 Spalten beschnitten.
- Kommen 5 s lang keine Leistungsdaten mehr (Programm beendet), schaltet der
  Arduino zurück in den normalen Betrieb.

| Instrument | Bedeutung                                                         |
|------------|-------------------------------------------------------------------|
| CPU        | Auslastung aller Kerne                                            |
| GPU        | Auslastung, siehe unten; ohne Quelle zeigt das Instrument `--`    |
| NET        | Datenrate (senden + empfangen) in % von `--net-max` (Standard 100 Mbit/s) |
| DISK       | Aktivzeit des am stärksten belasteten Laufwerks                   |

GPU-Quellen (die erste verfügbare wird verwendet):

- NVIDIA: `pip install nvidia-ml-py` oder `nvidia-smi` im Pfad
- AMD unter Linux: `/sys/class/drm/card*/device/gpu_busy_percent`
- Windows 10/11, alle Hersteller: Leistungsindikator „GPU Engine“ (3D)

**Bilder senden:** Der serielle Port kann nur von einem Programm geöffnet werden.
Deshalb sendet `pc_monitor.py` auch Bilder, automatisch in der halben Größe 70x128:

- `--image foto.jpg` sendet ein Bild beim Start,
- `--image-dir ORDNER` sendet alle `--image-interval` Sekunden (Standard 30) ein
  zufälliges Bild aus dem Ordner,
- während des Betriebs einen Dateipfad eintippen und Enter drücken.

## Protokoll

| Richtung       | Daten                                                         |
|----------------|---------------------------------------------------------------|
| PC → Arduino   | Leistungsdaten: `'P' 'R' 'F' <CPU> <GPU> <NET> <DISK>` (0–100 %, 255 = nicht verfügbar), keine Antwort |
| PC → Arduino   | Bildkopf: `'I' 'M' 'G' <Breite> <Höhe>` (je 1 Byte): 140x128, im Performance-Modus 70x128 |
| Arduino → PC   | `R` = bereit für den nächsten Block                           |
| PC → Arduino   | Block mit 30 Pixeln (60 Bytes), RGB565, High-Byte zuerst      |
| Arduino → PC   | `D` = Bild vollständig, `E` = Fehler (Größe / Zeitüberschreitung) |

Die Pixel werden zeilenweise von oben links gesendet. Der PC sendet jeden Block erst
nach einem `R`. So kann der 64-Byte-Empfangspuffer des UNO nicht überlaufen, auch
wenn er zwischendurch den Sensor ausliest.

# Classic Real Time Strategie Simulator

Eigener Sketch in `ClassicRealTimeStrategieSimulator/` für dieselbe Hardware
(Arduino UNO + 1.8" TFT-Shield, MPU6050 und SD-Karte werden nicht benötigt).
Zwei Parteien kämpfen automatisch gegeneinander:

- **BLAU** startet links, **ROT** rechts, jeweils mit 2 Panzern, 1 Artillerie und
  5 Soldaten. Jede Einheit ist höchstens 8x8 Pixel groß (7x7 Symbol + Lebensbalken).
- Hindernisse (grau/grün) werden per Zufall gesetzt. Panzer und Soldaten brauchen
  freie Sicht, die Artillerie schießt über Hindernisse hinweg.
- Die Statuszeile zeigt die verbleibenden Einheiten je Partei und die Spielzeit.
- Hat eine Partei verloren, zeigt das Display 10 s lang den Sieger, die Dauer und für
  jede Einheit, wie lange sie im Spiel war (`x` = ausgeschieden, `ok` = überlebt).
  Danach startet eine neue Schlacht mit neuen Zufallspositionen.

| Einheit    | Reichweite | Nachladen | Trefferchance |
|------------|------------|-----------|---------------|
| Soldat     | 10 px      | 0,5 s     | 50 %          |
| Panzer     | 30 px      | 1,6 s     | 60 %          |
| Artillerie | 50 px      | 3,0 s     | 40 %          |

Treffer bis zum Ausscheiden:

| Angreifer ↓ / Ziel → | Soldat | Panzer      | Artillerie |
|----------------------|--------|-------------|------------|
| Soldat               | 10     | kein Schaden | 10        |
| Panzer               | 1      | 4           | 2          |
| Artillerie           | 1      | 4           | 2          |

Soldaten, die nur noch Panzer als Gegner haben, weichen aus. Endet eine Schlacht
nach 3 Minuten nicht, gewinnt die Partei mit den meisten Lebenspunkten.
Alle Werte stehen oben im Sketch und lassen sich dort anpassen.

# Ant Simulation

Eigener Sketch in `AntSimulation/` für dieselbe Hardware (Arduino UNO + 1.8" TFT-Shield):

- Grüne Fläche mit einer braunen Ameisenkolonie (Radius 12 px) an zufälliger Stelle.
- Jede Sekunde schlüpft eine Ameise (1 schwarzer Pixel), höchstens 20 gleichzeitig.
  Sie läuft geradeaus in eine zufällige Richtung und wählt am Rand eine neue
  Richtung, mit der sie im Bild bleibt.
- Zufällig erscheint weißer Zucker (3 px). Die Ameise, die ihn findet, sendet ein
  Signal im Umkreis von 8 px. Jede Ameise, die es empfängt, kommt zum Zucker und
  hilft tragen – je mehr Träger, desto schneller wandert der Zucker zur Kolonie
  (bis 10 Träger).
- Im Ziel verschwinden Zucker und Träger in der Kolonie, oben rechts steigt der
  Punktestand. 3 s später erscheint neuer Zucker an einer freien Stelle.
- Oben links steht die Anzahl der geschlüpften Ameisen. Die Ameisen laufen nur
  unterhalb dieser Textzeile.

Alle Werte (Geschwindigkeiten, Radien, Zeiten, Farben) stehen oben im Sketch.
