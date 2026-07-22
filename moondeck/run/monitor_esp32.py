#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Monitor the ESP32 serial output. Saves to esp32/monitor.log."""

import argparse
import serial
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LOG_FILE = ROOT / "esp32" / "monitor.log"

# Shared moondeck.json + logLevel-toggle helpers (one level up, reachable from check/ and run/).
sys.path.insert(0, str(ROOT / "moondeck"))
from _moondeck_config import active_device_ips, set_log_level, LOG_INFO, LOG_WARN  # noqa: E402

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    args = parser.parse_args()

    # Raise the device(s) to Info so the tick line shows while monitoring; restore Warn on exit so the
    # status LED stops flickering on serial TX. Best-effort — an un-networked device is skipped (and
    # already logs at Info for its first 60 s anyway).
    ips = active_device_ips()
    set_log_level(ips, LOG_INFO)

    print(f"Monitoring {args.port} at {args.baud} baud...")
    print(f"Log saved to {LOG_FILE}")
    print("Press Ctrl+C (or Stop in MoonDeck) to stop.\n")
    sys.stdout.flush()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        set_log_level(ips, LOG_WARN)   # couldn't monitor; leave the device quiet
        sys.exit(1)

    with open(LOG_FILE, "w") as log:
        try:
            while True:
                line = ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    print(line)
                    sys.stdout.flush()
                    log.write(line + "\n")
                    log.flush()
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
            set_log_level(ips, LOG_WARN)   # back to quiet so the LED stops flickering
            print(f"\nStopped. Full log: {LOG_FILE}")

if __name__ == "__main__":
    main()
