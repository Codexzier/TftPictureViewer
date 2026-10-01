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

Mit `#define SAVE_RECEIVED_IMAGES 1` im Sketch werden empfangene Bilder zusätzlich
als `RCVnnn.BMP` auf der SD-Karte gespeichert und sind danach Teil der Diashow.
Das belegt ca. 2,3 KB Flash, der UNO ist damit zu über 99 % voll – deshalb ist es
standardmäßig aus.

### Protokoll

| Richtung       | Daten                                                         |
|----------------|---------------------------------------------------------------|
| PC → Arduino   | Kopf: `'I' 'M' 'G' <Breite=140> <Höhe=128>` (je 1 Byte)        |
| Arduino → PC   | `R` = bereit für den nächsten Block                           |
| PC → Arduino   | Block mit 30 Pixeln (60 Bytes), RGB565, High-Byte zuerst      |
| Arduino → PC   | `D` = Bild vollständig, `E` = Fehler (Größe / Zeitüberschreitung) |

Die Pixel werden zeilenweise von oben links gesendet. Der PC sendet jeden Block erst
nach einem `R`. So kann der 64-Byte-Empfangspuffer des UNO nicht überlaufen, auch
wenn er zwischendurch den Sensor ausliest.
