#!/usr/bin/env python3
"""Uploads an autopilot script to the console's title directory (/data/homebrew/<title id>/autopilot.txt) and reads it back.
The PS5 input backend (platform/ps5/input_ps5.cpp) ORs the buttons of that file into the pad state of the running title: see tools/autopilot/README.md.
usage: upload_autopilot.py <script> [--title-id PPSA88360] [--remove]
env:   RCOMP_PS5_HOST (required), RCOMP_PS5_FTP_PORT (default 2120)
--remove deletes the console's script instead (no script = no autopilot)."""
import argparse
import ftplib
import io
import os
import re
import sys
from pathlib import Path

STEP = re.compile(r"^\d+ [0-9A-Fa-f]+ \d+$")

p = argparse.ArgumentParser()
p.add_argument("script", type=Path, nargs="?")
p.add_argument("--title-id", default="PPSA88360")
p.add_argument("--remove", action="store_true")
args = p.parse_args()
if not re.fullmatch(r"PPSA\d{5}", args.title_id):
    sys.exit("FAIL bad title id")
remote = f"/data/homebrew/{args.title_id}/autopilot.txt"
host = os.environ.get("RCOMP_PS5_HOST", "")
if not host:
    sys.exit("BLOCKED set RCOMP_PS5_HOST to the console's address")

ftp = ftplib.FTP()
ftp.connect(host, int(os.environ.get("RCOMP_PS5_FTP_PORT", "2120")), timeout=15)
ftp.login()
if args.remove:
    try:
        ftp.delete(remote)
        print("removed", remote)
    except ftplib.error_perm as error:
        print("nothing to remove:", error)
    ftp.quit()
    sys.exit(0)

if args.script is None:
    sys.exit("FAIL a script is required (or --remove)")
data = args.script.read_bytes()
steps = 0
previous_start = -1
for number, line in enumerate(data.decode("ascii").splitlines(), 1):
    if not line.strip():
        continue
    if not STEP.match(line.strip()):
        sys.exit(f"FAIL line {number}: expected '<start_ms> <buttons_hex> <duration_ms>', got {line!r}")
    start = int(line.split()[0])
    if start < previous_start:
        print(f"note: line {number} starts before the previous step (steps may overlap; their buttons are ORed)")
    previous_start = start
    steps += 1
if not steps:
    sys.exit("FAIL the script has no step")

ftp.storbinary("STOR " + remote, io.BytesIO(data))
back = io.BytesIO()
ftp.retrbinary("RETR " + remote, back.write)
ftp.quit()
if back.getvalue() != data:
    sys.exit("FAIL readback differs from the uploaded script")
print(f"PASS {steps} steps uploaded to {remote} and read back ({len(data)} bytes)")
