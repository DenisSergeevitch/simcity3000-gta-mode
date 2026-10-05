#!/usr/bin/env python3
"""Capture the SimCity 3000 Wine window (works when it is behind other windows).
   tools/shot.py out.png [maxwidth]   -> writes out.png (full) and prints its size; maxwidth>0 also downsizes."""
import sys, subprocess, Quartz
from PIL import Image
out = sys.argv[1]; maxw = int(sys.argv[2]) if len(sys.argv) > 2 else 0
wid = None
for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID):
    if w.get('kCGWindowOwnerName') == 'wine' and w.get('kCGWindowName') == 'SimCity 3000':
        wid = w['kCGWindowNumber']; break
if wid is None: sys.exit("game window not found")
subprocess.run(['screencapture', '-x', '-o', '-l%d' % wid, out], check=True)
im = Image.open(out)
if maxw and im.width > maxw:
    im = im.resize((maxw, round(im.height * maxw / im.width)), Image.LANCZOS); im.save(out)
print(out, im.size)
