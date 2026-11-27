#!/usr/bin/env python3
"""Cattura la seriale USB-Serial/JTAG della Cardputer su file.

Uso:
    python tools/serial_log.py --seconds 15
    python tools/serial_log.py --seconds 60 --send r   # invia 'r' dopo 2 s

Il reset va fatto a parte: python -m esptool --chip esp32s3 --port COM5 --after hard-reset flash-id
Lo script riapre la porta da solo quando la USB si ri-enumera dopo il reset.
I log finiscono in tools/serial-<timestamp>.log (raw) e su stdout con timestamp.
"""
import argparse
import datetime
import sys
import time

import serial


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = True   # host "collegato": la CDC del firmware puo' trasmettere
    ser.rts = False
    ser.open()
    return ser


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--send", default=None, help="carattere da inviare dopo 2 secondi")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or f"tools/serial-{stamp}.log"

    deadline = time.monotonic() + args.seconds
    start = time.monotonic()
    ser = None
    sent = False
    pending = b""
    total = 0
    print(f"# cattura su {args.port} per {args.seconds:.0f}s -> {out}", flush=True)
    with open(out, "wb") as f:
        while time.monotonic() < deadline:
            if ser is None:
                try:
                    ser = open_port(args.port, args.baud)
                    print("# porta aperta", flush=True)
                except serial.SerialException:
                    time.sleep(0.15)
                    continue
            if args.send and not sent and time.monotonic() - start > 2.0:
                try:
                    ser.write(args.send.encode())
                    sent = True
                except serial.SerialException:
                    pass
            try:
                chunk = ser.read(256)
            except serial.SerialException:
                # Reset / ri-enumerazione USB: riapri la nuova istanza.
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.15)
                continue
            if not chunk:
                continue
            total += len(chunk)
            f.write(chunk)
            f.flush()
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                stamp_ms = int((time.monotonic() - start) * 1000)
                print(f"[{stamp_ms:7d}ms] {line.decode('utf-8', 'replace').rstrip()}", flush=True)
        if pending:
            print(f"[  end  ] {pending.decode('utf-8', 'replace').rstrip()}", flush=True)
    if ser is not None:
        ser.close()
    print(f"# {total} byte salvati in {out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
