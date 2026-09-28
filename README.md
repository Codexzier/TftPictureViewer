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

Der Sensor ist auf maximale Empfindlichkeit eingestellt (±2 g, ±250 °/s,
Tiefpass ~94 Hz, 100 Hz Abtastung). Die Balken verwenden eine Wurzelkennlinie,
damit auch kleinste Vibrationen sichtbar sind.

## Verdrahtung

| Modul            | Pin        | Arduino UNO |
|------------------|------------|-------------|
| TFT              | CS         | D10         |
| TFT              | A0 / DC    | D8          |
| TFT              | RESET      | D9          |
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
