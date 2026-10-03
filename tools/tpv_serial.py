"""Gemeinsame Funktionen fuer die serielle Verbindung zum Picture Viewer with Sense.

Wird von send_image.py und pc_monitor.py verwendet. Das Protokoll ist im
Sketch und in der README beschrieben.
"""

import time

from PIL import Image, ImageOps

BAUD = 250000
FULL_SIZE = (140, 128)   # Bildbereich normal
HALF_SIZE = (70, 128)    # Bildbereich im Performance-Modus
BLOCK_PIXELS = 30        # muss zu SERIAL_BLOCK_PIXELS im Sketch passen
BOOT_TIMEOUT_S = 15      # Kalibrierung + Titel nach dem Reset
REPLY_TIMEOUT_S = 5      # Arduino laedt evtl. gerade ein Bild von der SD-Karte
NOT_AVAILABLE = 255      # Leistungswert nicht verfuegbar


def image_to_rgb565(path, size):
    """Laedt ein Bild, passt es auf size an (mittig beschnitten) und liefert RGB565-Daten."""
    with Image.open(path) as img:
        img = ImageOps.exif_transpose(img).convert("RGB")
        if img.size != size:
            img = ImageOps.fit(img, size, Image.LANCZOS)
        rgb = img.tobytes()
    data = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data += bytes((value >> 8, value & 0xFF))   # High-Byte zuerst
    return bytes(data)


def wait_ready(port):
    """Das Oeffnen des Ports setzt den UNO zurueck -> auf 'TPV READY' warten."""
    deadline = time.time() + BOOT_TIMEOUT_S
    received = b""
    while time.time() < deadline:
        received += port.read(port.in_waiting or 1)
        if b"TPV READY" in received:
            return True
    return False


def send_image(port, data, size, progress=None):
    """Sendet RGB565-Daten der Groesse size. Wirft RuntimeError bei Fehlern."""
    width, height = size
    block_size = BLOCK_PIXELS * 2
    position = 0
    port.reset_input_buffer()
    port.write(b"IMG" + bytes((width, height)))

    while True:
        reply = port.read(1)
        if not reply:
            raise RuntimeError("keine Antwort vom Arduino (Zeitueberschreitung)")
        if reply == b"R":
            port.write(data[position:position + block_size])
            position += block_size
            if progress:
                progress(min(position, len(data)) * 100 // len(data))
        elif reply == b"D":
            return
        elif reply == b"E":
            raise RuntimeError("der Arduino meldet einen Fehler (falsche Bildgroesse?)")
        # andere Zeichen (z.B. Startmeldung) ignorieren


def _percent_byte(value):
    if value is None:
        return NOT_AVAILABLE
    return int(round(min(100.0, max(0.0, value))))


def send_perf(port, cpu, gpu, net, disk):
    """Sendet Auslastungswerte in Prozent (None = nicht verfuegbar)."""
    port.write(b"PRF" + bytes(_percent_byte(v) for v in (cpu, gpu, net, disk)))
