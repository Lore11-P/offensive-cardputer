#!/usr/bin/env python3
"""Invia un file alla Cardputer via USB CDC.

Uso:
  python tools/usb_put.py COM5 foto.jpg
  python tools/usb_put.py COM5 foto.jpg ir/tv/foto.jpg

Sul device aprire Sistema > USB file: la schermata stampa
"ORBITPUT READY" e resta in attesa di un header.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("Errore: manca pyserial. Installa con: python -m pip install pyserial",
          file=sys.stderr)
    sys.exit(2)

MAGIC = b"ORBITPUT"
VERSION = 1
HEADER = struct.Struct("<8sBHII")
CHUNK = 8192
MAX_PATH = 160
MAX_FILE = 64 * 1024 * 1024
DEVICE_HINT = ("Apri Sistema > USB file sul Cardputer (deve mostrare "
               "'Collega il PC e avvia tools/usb_put.py') e riprova.")

ERR_TEXT = {
    "ERR magic": "header non riconosciuto dal device",
    "ERR version": "versione protocollo non supportata dal firmware",
    "ERR path": "path rifiutato dal device (niente '..', ':' o '//')",
    "ERR size": "dimensione rifiutata (1 byte - 64 MB)",
    "ERR sd": "microSD non montata sul device",
    "ERR write": "scrittura sulla microSD fallita",
    "ERR crc": "CRC non corrisponde: trasferimento corrotto",
    "ERR timeout": "timeout del device",
    "ERR cancel": "trasferimento annullato sul device (ESC)",
}


class DeviceError(RuntimeError):
    pass


class TransferTimeout(TimeoutError):
    pass


def read_line(ser: "serial.Serial", deadline: float) -> str | None:
    """Legge una riga entro deadline; None se scade. Ignora le righe di log."""
    buf = bytearray()
    while time.monotonic() < deadline:
        byte = ser.read(1)
        if not byte:
            continue
        if byte == b"\n":
            text = buf.decode("utf-8", "replace").strip("\r")
            buf.clear()
            if text:
                return text
        else:
            buf.extend(byte)
    return None


def wait_reply(ser: "serial.Serial", expected: str, timeout: float,
               ignore_timeout: bool = False) -> None:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TransferTimeout(
                f"timeout in attesa di {expected} dal device. {DEVICE_HINT}")
        line = read_line(ser, time.monotonic() + min(remaining, 0.5))
        if line is None:
            continue
        if line == expected:
            return
        if line.startswith("ERR"):
            # Un ERR timeout emesso mentre il device era in attesa non deve
            # far fallire l'header appena inviato.
            if ignore_timeout and line == "ERR timeout":
                continue
            raise DeviceError(ERR_TEXT.get(line, f"errore del device: {line}"))
        # Riga di log o "ORBITPUT READY": ignorata.


def file_crc32(path: str) -> int:
    crc = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(1024 * 1024)
            if not block:
                break
            crc = zlib.crc32(block, crc)
    return crc & 0xFFFFFFFF


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Invia un file alla Cardputer via USB CDC (Sistema > USB file).")
    parser.add_argument("port", help="porta seriale, es. COM5 o /dev/ttyACM0")
    parser.add_argument("local", help="file locale da inviare")
    parser.add_argument("remote", nargs="?",
                        help="path remoto sulla microSD (default: nome del file)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="baud rate (USB CDC, default 115200)")
    args = parser.parse_args()

    if not os.path.isfile(args.local):
        print(f"Errore: file non trovato: {args.local}", file=sys.stderr)
        return 1
    size = os.path.getsize(args.local)
    if size == 0:
        print("Errore: il file e' vuoto.", file=sys.stderr)
        return 1
    if size > MAX_FILE:
        print(f"Errore: file troppo grande ({size} byte, massimo {MAX_FILE}).",
              file=sys.stderr)
        return 1

    remote = args.remote if args.remote else os.path.basename(args.local)
    remote_bytes = remote.encode("utf-8")
    if not remote_bytes or len(remote_bytes) > MAX_PATH:
        print(f"Errore: path remoto vuoto o oltre {MAX_PATH} byte UTF-8.",
              file=sys.stderr)
        return 1

    print(f"Calcolo CRC-32 di {args.local} ({size} byte)...")
    crc = file_crc32(args.local)
    header = HEADER.pack(MAGIC, VERSION, len(remote_bytes), size, crc) + remote_bytes

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=10)
    except serial.SerialException as exc:
        print(f"Errore: impossibile aprire {args.port}: {exc}", file=sys.stderr)
        return 1

    try:
        ser.reset_input_buffer()
        print(f"Invio header per {remote} ({size} byte, crc 0x{crc:08X})...")
        ser.write(header)
        ser.flush()
        # 20 s: il timeout dell'header sul device e' 15 s.
        wait_reply(ser, "OK", 20, ignore_timeout=True)

        sent = 0
        with open(args.local, "rb") as f:
            while sent < size:
                chunk = f.read(min(CHUNK, size - sent))
                if not chunk:
                    raise OSError("file locale troncato durante la lettura")
                ser.write(chunk)
                ser.flush()
                wait_reply(ser, "ACK", 15)
                sent += len(chunk)
                percent = sent * 100 // size
                print(f"\r  {percent:3d}%  {sent}/{size} byte", end="", flush=True)
        print()
        # Il device verifica il CRC, rinomina il .part e risponde DONE.
        wait_reply(ser, "DONE", 15)
        print(f"OK: /{remote.lstrip('/')} scritto sulla microSD.")
        return 0
    except DeviceError as exc:
        print(f"\nErrore: {exc}", file=sys.stderr)
        return 1
    except TransferTimeout as exc:
        print(f"\nErrore: {exc}", file=sys.stderr)
        return 1
    except (serial.SerialException, OSError) as exc:
        print(f"\nErrore di comunicazione: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrotto.", file=sys.stderr)
        return 130
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
