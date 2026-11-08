"""Turn the last [lcd] console frame into an enlarged PNG."""
import argparse
from pathlib import Path
import re
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
frames = re.findall(r"\[lcd\] ([0-9a-f]{1008})", args.log.read_text(errors="replace"))
if not frames:
    raise SystemExit("No [lcd] frame found")
fb = bytes.fromhex(frames[-1])
frame = Image.new("RGB", (84, 48), "white")
pixels = frame.load()
for y in range(48):
    for x in range(84):
        if fb[(y // 8) * 84 + x] & (1 << (y % 8)):
            pixels[x, y] = (0, 0, 0)
frame.resize((504, 288), Image.Resampling.NEAREST).save(args.output)
