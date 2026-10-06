#!/usr/bin/env python3
"""Fetches every numbered capture (frame_NNN.ppm, half size) of the last run from the console's title directory into <out dir>, converts them to PNG, and writes a contact sheet.
usage: fetch_all_frames.py <out dir> [--columns 6] [--first 1] [--last 240]
env:   RCOMP_PS5_HOST (required), RCOMP_PS5_FTP_PORT (default 2120)"""
import os
import argparse
import ftplib
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument("out", type=Path)
p.add_argument("--columns", type=int, default=6)
p.add_argument("--first", type=int, default=1)
p.add_argument("--last", type=int, default=240)
args = p.parse_args()
host = os.environ.get("RCOMP_PS5_HOST", "")
if not host:
    sys.exit("BLOCKED set RCOMP_PS5_HOST to the console's address")
args.out.mkdir(parents=True, exist_ok=True)

ftp = ftplib.FTP()
ftp.connect(host, int(os.environ.get("RCOMP_PS5_FTP_PORT", "2120")), timeout=30)
ftp.login()
ftp.cwd("/data/homebrew/" + os.environ.get("RCOMP_TITLE_ID", "PPSA88360"))
names = sorted(n.rsplit("/", 1)[-1] for n in ftp.nlst())
frames = [n for n in names if re.fullmatch(r"frame_\d{3}\.ppm", n) and args.first <= int(n[6:9]) <= args.last]
print(len(frames), "numbered captures on the console")
thumbs = []
for name in frames:
    path = args.out / name
    with path.open("wb") as handle:
        ftp.retrbinary("RETR " + name, handle.write)
    try:
        image = Image.open(path).convert("RGB")
    except OSError as error:
        print("incomplete capture", name, error)
        continue
    image.save(path.with_suffix(".png"))
    thumbs.append((int(name[6:9]), image.resize((320, 180))))
    path.unlink()
ftp.quit()

if thumbs:
    columns = args.columns
    rows = (len(thumbs) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * 320, rows * 180), (40, 0, 40))
    draw = ImageDraw.Draw(sheet)
    for i, (n, thumb) in enumerate(thumbs):
        x, y = (i % columns) * 320, (i // columns) * 180
        sheet.paste(thumb, (x, y))
        draw.rectangle([x, y, x + 52, y + 14], fill=(0, 0, 0))
        draw.text((x + 3, y + 1), f"#{n}", fill=(255, 255, 0))
    sheet.save(args.out / "contact_sheet.png")
    print("contact sheet", sheet.size, "of", len(thumbs), "frames")
