#!/usr/bin/env python3
"""Replay a QNX serial log in the terminal at real-time speed.

Used to record the demo video: the log is the real output of a live
QNX 8.0 run in QEMU (see demo-90s.log), replayed line by line so the
screen recording shows the dashboard exactly as it appeared.

Usage: replay-log.py <logfile> [speed]
  speed: playback speed multiplier (default 1.0)
"""
import sys
import time

log = sys.argv[1]
speed = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0

with open(log, "r", errors="replace") as f:
    lines = f.readlines()

# Skip QNX boot noise before the app banner for a tighter video.
start = 0
for i, line in enumerate(lines):
    if "QNX Real-Time Patient Monitor" in line:
        start = max(0, i - 1)
        break
lines = lines[start:]

# The dashboard prints once per second; replay at that pace.
for line in lines:
    sys.stdout.write(line)
    sys.stdout.flush()
    if line.startswith("[t="):
        time.sleep(1.0 / speed)
    elif "SUMMARY" in line or line.startswith("thread") or \
            line.startswith("alarm") or line.startswith("ecg") or \
            line.startswith("spo2") or line.startswith("nibp") or \
            line.startswith("display") or line.startswith("verdict"):
        time.sleep(0.4 / speed)
