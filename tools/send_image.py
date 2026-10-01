#!/usr/bin/env python3
"""Sendet ein Bild ueber die serielle Schnittstelle an den Picture Viewer with Sense.

Das Bild wird in der Zielaufloesung des Bildbereichs (140x128) als RGB565
uebertragen (35.840 Bytes). Bilder mit anderer Groesse werden vorher skaliert
und mittig beschnitten.

Benutzung:
    pip install pyserial Pillow
    python3 send_image.py <port> <bild> [--baud 250000] [--no-wait]

Beispiele:
    python3 send_image.py COM3 foto.jpg
    python3 send_image.py /dev/ttyACM0 foto.jpg

Hinweis: Das Oeffnen des Ports setzt den Arduino UNO zurueck. Das Skript
wartet deshalb, bis Kalibrierung und Titel durchgelaufen sind ("TPV READY").
"""

import argparse
import sys
import time

import serial
from PIL import Image, ImageOps

WIDTH = 140
HEIGHT = 128
BLOCK_PIXELS = 30        # muss zu SERIAL_BLOCK_PIXELS im Sketch passen
BOOT_TIMEOUT_S = 15      # Kalibrierung + Titel nach dem Reset
REPLY_TIMEOUT_S = 5      # Arduino laedt evtl. gerade ein Bild von der SD-Karte


def to_rgb565(path: str) -> bytes:
    with Image.open(path) as img:
        img = ImageOps.exif_transpose(img).convert("RGB")
        if img.size != (WIDTH, HEIGHT):
            img = ImageOps.fit(img, (WIDTH, HEIGHT), Image.LANCZOS)
        rgb = img.tobytes()
    data = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data += bytes((value >> 8, value & 0xFF))   # High-Byte zuerst
    return bytes(data)


def wait_ready(port: serial.Serial) -> bool:
    deadline = time.time() + BOOT_TIMEOUT_S
    received = b""
    while time.time() < deadline:
        received += port.read(port.in_waiting or 1)
        if b"TPV READY" in received:
            return True
    return False


def send(port: serial.Serial, data: bytes) -> None:
    block_size = BLOCK_PIXELS * 2
    position = 0
    port.reset_input_buffer()
    port.write(b"IMG" + bytes((WIDTH, HEIGHT)))

    while True:
        reply = port.read(1)
        if not reply:
            raise RuntimeError("keine Antwort vom Arduino (Zeitueberschreitung)")
        if reply == b"R":
            port.write(data[position:position + block_size])
            position += block_size
            percent = min(position, len(data)) * 100 // len(data)
            print(f"\r{percent:3d} %", end="", flush=True)
        elif reply == b"D":
            print()
            return
        elif reply == b"E":
            print()
            raise RuntimeError("der Arduino meldet einen Fehler")
        # andere Zeichen (z.B. Startmeldung) ignorieren


def main() -> int:
    parser = argparse.ArgumentParser(description="Bild an den Picture Viewer senden")
    parser.add_argument("port", help="serielle Schnittstelle, z.B. COM3 oder /dev/ttyACM0")
    parser.add_argument("image", help="Bilddatei (JPEG, PNG, BMP, ...)")
    parser.add_argument("--baud", type=int, default=250000)
    parser.add_argument("--no-wait", action="store_true",
                        help="nicht auf die Startmeldung warten (kein Reset beim Oeffnen)")
    args = parser.parse_args()

    data = to_rgb565(args.image)

    with serial.Serial(args.port, args.baud, timeout=REPLY_TIMEOUT_S) as port:
        if not args.no_wait:
            print("Warte auf den Arduino ...")
            if not wait_ready(port):
                print("Keine Startmeldung empfangen, versuche es trotzdem.")
        start = time.time()
        try:
            send(port, data)
        except RuntimeError as error:
            print(f"Fehler: {error}")
            return 1
        print(f"{len(data)} Bytes in {time.time() - start:.1f} s uebertragen.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
