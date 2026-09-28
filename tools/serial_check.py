"""Capture a short boot log after toggling the board reset line."""

import argparse
import time

import serial


parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM8")
parser.add_argument("--seconds", type=float, default=12)
args = parser.parse_args()

with serial.Serial(args.port, 115200, timeout=0.5) as port:
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False
    until = time.monotonic() + args.seconds
    while time.monotonic() < until:
        line = port.readline()
        if line:
            print(line.decode("utf-8", errors="replace"), end="", flush=True)
