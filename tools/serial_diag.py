"""Console seriale per la diagnostica di OrbitOS (COM5).

La scheda stampa log e risposte [DIAG]; i comandi si inviano come righe.
Esempi:
    python tools/serial_diag.py --cmd heap --cmd "run 2048" --seconds 8
    python tools/serial_diag.py -i          # prompt interattivo
    echo "heap" | python tools/serial_diag.py
"""
import argparse
import sys
import threading
import time

import serial


def reader(port, stop):
    while not stop.is_set():
        try:
            line = port.readline()
        except serial.SerialException:
            break
        if line:
            sys.stdout.write(line.decode("utf-8", "replace"))
            sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--cmd", action="append", default=[],
                        help="comando da inviare (ripetibile)")
    parser.add_argument("--seconds", type=float, default=3.0,
                        help="attesa dopo ogni --cmd")
    parser.add_argument("-i", "--interactive", action="store_true")
    args = parser.parse_args()

    port = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.5)  # la CDC si riapre dopo il reset: evita di perdere la prima riga
    stop = threading.Event()
    thread = threading.Thread(target=reader, args=(port, stop), daemon=True)
    thread.start()

    def send(text):
        port.write((text.strip() + "\r\n").encode())

    if args.cmd:
        for command in args.cmd:
            print(f"# > {command}")
            send(command)
            time.sleep(args.seconds)

    if args.interactive or (not args.cmd and sys.stdin.isatty()):
        print("# comandi: heap, fb, fb rel, fb res, games, run 2048, stop, tasks, wifi, reset")
        for line in sys.stdin:
            if line.strip():
                send(line)
    elif not args.cmd:
        for line in sys.stdin:
            if line.strip():
                send(line)
                time.sleep(args.seconds)

    stop.set()
    time.sleep(0.3)
    port.close()


if __name__ == "__main__":
    main()
