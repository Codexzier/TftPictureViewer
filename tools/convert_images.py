#!/usr/bin/env python3
"""Wandelt JPEG-Bilder fuer den Picture Viewer with Sense um.

Der Arduino UNO hat zu wenig RAM, um JPEGs direkt zu dekodieren. Dieses Skript
skaliert jedes Bild auf den Bildbereich des TFT (140x128 Pixel, mittig
beschnitten) und speichert es als unkomprimiertes 24-Bit-BMP mit einem
8.3-Dateinamen (IMG001.BMP, IMG002.BMP, ...).

Benutzung:
    pip install Pillow
    python3 convert_images.py <jpeg-ordner> <ausgabe-ordner>

Danach den Inhalt des Ausgabe-Ordners ins Hauptverzeichnis der
(FAT16/FAT32-formatierten) SD-Karte kopieren.
"""

import sys
from pathlib import Path

from PIL import Image, ImageOps

WIDTH = 140
HEIGHT = 128
EXTENSIONS = {".jpg", ".jpeg", ".png"}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)

    files = sorted(p for p in src.iterdir() if p.suffix.lower() in EXTENSIONS)
    if not files:
        print(f"Keine Bilder in {src} gefunden.")
        return 1

    for number, path in enumerate(files, start=1):
        with Image.open(path) as img:
            img = ImageOps.exif_transpose(img).convert("RGB")
            img = ImageOps.fit(img, (WIDTH, HEIGHT), Image.LANCZOS)
            name = f"IMG{number:03d}.BMP"
            img.save(dst / name, format="BMP")
            print(f"{path.name} -> {name}")

    print(f"{len(files)} Bilder umgewandelt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
