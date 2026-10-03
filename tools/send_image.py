#!/usr/bin/env python3
"""Sendet ein Bild ueber die serielle Schnittstelle an den Picture Viewer with Sense.

Das Bild wird in der Zielaufloesung des Bildbereichs (140x128) als RGB565
uebertragen (35.840 Bytes). Bilder mit anderer Groesse werden vorher skaliert
und mittig beschnitten.

Laeuft bereits pc_monitor.py, belegt dieses den Port. Bilder dann direkt dort
senden (Dateipfad eintippen oder --image / --image-dir).

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

import tpv_serial


def main() -> int:
    parser = argparse.ArgumentParser(description="Bild an den Picture Viewer senden")
    parser.add_argument("port", help="serielle Schnittstelle, z.B. COM3 oder /dev/ttyACM0")
    parser.add_argument("image", help="Bilddatei (JPEG, PNG, BMP, ...)")
    parser.add_argument("--baud", type=int, default=tpv_serial.BAUD)
    parser.add_argument("--no-wait", action="store_true",
                        help="nicht auf die Startmeldung warten (kein Reset beim Oeffnen)")
    args = parser.parse_args()

    data = tpv_serial.image_to_rgb565(args.image, tpv_serial.FULL_SIZE)

    with serial.Serial(args.port, args.baud, timeout=tpv_serial.REPLY_TIMEOUT_S) as port:
        if not args.no_wait:
            print("Warte auf den Arduino ...")
            if not tpv_serial.wait_ready(port):
                print("Keine Startmeldung empfangen, versuche es trotzdem.")
        start = time.time()
        try:
            tpv_serial.send_image(port, data, tpv_serial.FULL_SIZE,
                                  lambda p: print(f"\r{p:3d} %", end="", flush=True))
        except RuntimeError as error:
            print(f"\nFehler: {error}")
            return 1
        print(f"\n{len(data)} Bytes in {time.time() - start:.1f} s uebertragen.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
