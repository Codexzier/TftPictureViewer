#!/usr/bin/env python3
"""Sendet die Auslastung von CPU, GPU, Netzwerk und Festplatte an den
Picture Viewer with Sense.

Solange Daten ankommen, zeigt der Arduino 4 runde Analog-Anzeigen und die
Diashow nur noch auf halber Breite (70x128). Endet das Programm, schaltet der
Arduino nach 5 s wieder in den normalen Betrieb.

Bilder werden ueber dieselbe Verbindung gesendet (der Port kann nur von einem
Programm geoeffnet werden) - automatisch in der halben Groesse 70x128:
    --image DATEI       einmal beim Start
    --image-dir ORDNER  zufaellig alle --image-interval Sekunden
    Dateipfad eintippen + Enter, waehrend das Programm laeuft

Benutzung:
    pip install pyserial Pillow psutil
    python3 pc_monitor.py COM3
    python3 pc_monitor.py /dev/ttyACM0 --image-dir bilder/ --net-max 250

GPU-Auslastung (die erste verfuegbare Quelle wird verwendet):
    - NVIDIA: Python-Paket nvidia-ml-py (pip install nvidia-ml-py) oder nvidia-smi
    - AMD unter Linux: /sys/class/drm/card*/device/gpu_busy_percent
    - Windows 10/11 (alle Hersteller): Leistungsindikator "GPU Engine" (3D)
    Ohne Quelle zeigt das GPU-Instrument "--".

Werte:
    CPU   Auslastung aller Kerne in %
    GPU   Auslastung in %
    NET   Datenrate (senden + empfangen) in % von --net-max (Mbit/s)
    DISK  Aktivzeit des am staerksten belasteten Laufwerks in %
"""

import argparse
import glob
import queue
import random
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import psutil
except ImportError:
    sys.exit("Bitte zuerst installieren: pip install psutil pyserial Pillow")
import serial

import tpv_serial

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"}


# ---------------------------------------------------------------------------
# GPU
# ---------------------------------------------------------------------------
class _WindowsGpuCounter:
    """GPU-Auslastung ueber den Windows-Leistungsindikator "GPU Engine" (3D)."""

    PATH = r"\GPU Engine(*engtype_3D)\Utilization Percentage"
    PDH_FMT_DOUBLE = 0x00000200
    PDH_MORE_DATA = 0x800007D2

    def __init__(self):
        import ctypes
        from ctypes import wintypes

        class Value(ctypes.Structure):
            _fields_ = [("CStatus", wintypes.DWORD), ("doubleValue", ctypes.c_double)]

        class Item(ctypes.Structure):
            _fields_ = [("szName", wintypes.LPWSTR), ("FmtValue", Value)]

        self._ctypes = ctypes
        self._item = Item
        self._pdh = ctypes.WinDLL("pdh")
        self._query = ctypes.c_void_p()
        self._counter = ctypes.c_void_p()
        if self._pdh.PdhOpenQueryW(None, 0, ctypes.byref(self._query)) != 0:
            raise OSError("PdhOpenQuery fehlgeschlagen")
        if self._pdh.PdhAddEnglishCounterW(self._query, self.PATH, 0,
                                           ctypes.byref(self._counter)) != 0:
            raise OSError("GPU-Leistungsindikator nicht vorhanden")
        self._pdh.PdhCollectQueryData(self._query)

    def read(self):
        ctypes = self._ctypes
        if self._pdh.PdhCollectQueryData(self._query) != 0:
            return None
        size = ctypes.c_ulong(0)
        count = ctypes.c_ulong(0)
        status = self._pdh.PdhGetFormattedCounterArrayW(
            self._counter, self.PDH_FMT_DOUBLE, ctypes.byref(size), ctypes.byref(count), None)
        if status & 0xFFFFFFFF != self.PDH_MORE_DATA:
            return 0.0   # gerade keine 3D-Engine aktiv
        buffer = (ctypes.c_byte * size.value)()
        status = self._pdh.PdhGetFormattedCounterArrayW(
            self._counter, self.PDH_FMT_DOUBLE, ctypes.byref(size), ctypes.byref(count), buffer)
        if status != 0:
            return None
        items = ctypes.cast(buffer, ctypes.POINTER(self._item))
        total = sum(items[i].FmtValue.doubleValue for i in range(count.value)
                    if items[i].FmtValue.CStatus in (0, 1))
        return min(100.0, total)


def detect_gpu():
    """Liefert (Name der Quelle, Lesefunktion) oder (None, None)."""
    try:
        import pynvml
        pynvml.nvmlInit()
        handles = [pynvml.nvmlDeviceGetHandleByIndex(i)
                   for i in range(pynvml.nvmlDeviceGetCount())]
        if handles:
            return "NVIDIA (NVML)", lambda: max(
                pynvml.nvmlDeviceGetUtilizationRates(h).gpu for h in handles)
    except Exception:
        pass

    if shutil.which("nvidia-smi"):
        def read_smi():
            output = subprocess.run(
                ["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=3).stdout
            return max(float(line) for line in output.split())
        try:
            read_smi()
            return "NVIDIA (nvidia-smi)", read_smi
        except Exception:
            pass

    paths = glob.glob("/sys/class/drm/card*/device/gpu_busy_percent")
    if paths:
        def read_sysfs():
            return max(float(Path(p).read_text()) for p in paths)
        try:
            read_sysfs()
            return "Linux sysfs (gpu_busy_percent)", read_sysfs
        except Exception:
            pass

    if sys.platform == "win32":
        try:
            counter = _WindowsGpuCounter()
            counter.read()
            return "Windows-Leistungsindikator (GPU Engine 3D)", counter.read
        except Exception:
            pass

    return None, None


# ---------------------------------------------------------------------------
# Messwerte
# ---------------------------------------------------------------------------
def _is_loopback(name):
    return name == "lo" or name.lower().startswith("loopback")


class Metrics:
    def __init__(self, net_max_mbit):
        self.net_max_bytes = net_max_mbit * 1_000_000 / 8
        self.gpu_source, self._read_gpu = detect_gpu()
        psutil.cpu_percent(None)   # erster Aufruf startet die Messung
        self._time = time.monotonic()
        self._net = self._net_bytes()
        self._disk = self._disk_busy_ms()

    @staticmethod
    def _net_bytes():
        counters = psutil.net_io_counters(pernic=True)
        return sum(c.bytes_sent + c.bytes_recv
                   for name, c in counters.items() if not _is_loopback(name))

    @staticmethod
    def _disk_busy_ms():
        """Aktivzeit je Laufwerk in ms (Linux: busy_time, sonst Lese- + Schreibzeit)."""
        try:
            counters = psutil.disk_io_counters(perdisk=True) or {}
        except Exception:
            return {}
        result = {}
        for name, c in counters.items():
            busy = getattr(c, "busy_time", None)
            result[name] = busy if busy is not None else c.read_time + c.write_time
        return result

    def read(self):
        """Liefert (cpu, gpu, net, disk) in Prozent, None = nicht verfuegbar."""
        now = time.monotonic()
        elapsed = max(now - self._time, 0.001)
        self._time = now

        cpu = psutil.cpu_percent(None)

        gpu = None
        if self._read_gpu:
            try:
                gpu = self._read_gpu()
            except Exception:
                gpu = None

        net_bytes = self._net_bytes()
        net = (net_bytes - self._net) / elapsed / self.net_max_bytes * 100
        self._net = net_bytes

        disk_busy = self._disk_busy_ms()
        disk = None
        if disk_busy:
            disk = max((busy - self._disk.get(name, busy)) / (elapsed * 1000) * 100
                       for name, busy in disk_busy.items())
        self._disk = disk_busy

        return cpu, gpu, max(net, 0.0), disk if disk is None else max(disk, 0.0)


# ---------------------------------------------------------------------------
# Bilder
# ---------------------------------------------------------------------------
def pick_image(folder, last):
    files = [p for p in Path(folder).iterdir() if p.suffix.lower() in IMAGE_EXTENSIONS]
    if len(files) > 1 and last in files:
        files.remove(last)
    return random.choice(files) if files else None


def read_console(requests):
    """Eingetippte Dateipfade als Bild senden."""
    for line in sys.stdin:
        path = line.strip().strip('"')
        if path:
            requests.put(Path(path))


def send_image_file(port, path):
    try:
        data = tpv_serial.image_to_rgb565(path, tpv_serial.HALF_SIZE)
    except Exception as error:
        print(f"Bild {path} nicht lesbar: {error}")
        return
    start = time.time()
    try:
        tpv_serial.send_image(port, data, tpv_serial.HALF_SIZE)
        print(f"Bild {path.name} gesendet ({time.time() - start:.1f} s).")
    except RuntimeError as error:
        print(f"Bild {path.name}: {error}")


def format_value(value):
    return " -- " if value is None else f"{min(value, 100):3.0f}%"


# ---------------------------------------------------------------------------
# Hauptprogramm
# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Leistungsdaten an den Picture Viewer senden",
        epilog="Waehrend des Betriebs: Bildpfad eintippen + Enter sendet das Bild.")
    parser.add_argument("port", help="serielle Schnittstelle, z.B. COM3 oder /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=tpv_serial.BAUD)
    parser.add_argument("--interval", type=float, default=1.0,
                        help="Sekunden zwischen zwei Messungen (Standard 1, max. 4)")
    parser.add_argument("--net-max", type=float, default=100.0,
                        help="Netzwerk-Vollausschlag in Mbit/s (Standard 100)")
    parser.add_argument("--image", type=Path, help="Bild beim Start senden")
    parser.add_argument("--image-dir", type=Path, help="Ordner fuer eine Diashow vom PC")
    parser.add_argument("--image-interval", type=float, default=30.0,
                        help="Sekunden zwischen Bildern aus --image-dir (Standard 30)")
    parser.add_argument("--no-wait", action="store_true",
                        help="nicht auf die Startmeldung warten (kein Reset beim Oeffnen)")
    args = parser.parse_args()
    interval = min(max(args.interval, 0.2), 4.0)   # Arduino-Timeout: 5 s

    metrics = Metrics(args.net_max)
    print(f"GPU-Quelle: {metrics.gpu_source or 'keine gefunden (Anzeige --)'}")

    requests = queue.Queue()
    if args.image:
        requests.put(args.image)
    threading.Thread(target=read_console, args=(requests,), daemon=True).start()

    with serial.Serial(args.port, args.baud, timeout=tpv_serial.REPLY_TIMEOUT_S) as port:
        if not args.no_wait:
            print("Warte auf den Arduino ...")
            if not tpv_serial.wait_ready(port):
                print("Keine Startmeldung empfangen, sende trotzdem.")
        print("Sende Leistungsdaten (Strg+C beendet). Bildpfad + Enter sendet ein Bild.")

        last_image = None
        next_dir_image = time.monotonic() + 2   # erst nach dem Umschalten
        next_status = 0.0
        try:
            while True:
                cycle_start = time.monotonic()
                values = metrics.read()
                tpv_serial.send_perf(port, *values)

                if cycle_start >= next_status:
                    print("CPU {} | GPU {} | NET {} | DISK {}".format(
                        *(format_value(v) for v in values)))
                    next_status = cycle_start + 10

                # Bilder erst nach den Leistungsdaten senden: der Arduino ist
                # dann sicher im Performance-Modus und erwartet 70x128
                try:
                    path = requests.get_nowait()
                except queue.Empty:
                    path = None
                    if args.image_dir and cycle_start >= next_dir_image:
                        path = last_image = pick_image(args.image_dir, last_image)
                        next_dir_image = cycle_start + args.image_interval
                if path:
                    send_image_file(port, path)

                time.sleep(max(0.0, interval - (time.monotonic() - cycle_start)))
        except KeyboardInterrupt:
            print("\nBeendet - der Arduino schaltet nach 5 s zurueck.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
