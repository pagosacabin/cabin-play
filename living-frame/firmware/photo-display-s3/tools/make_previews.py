#!/usr/bin/env python3
"""Rebuild the committed preview PNGs from tools/host_render.cpp's PPM output.

    g++ -O2 -Isrc tools/host_render.cpp -o /tmp/photo_s3_render -lm
    /tmp/photo_s3_render tools
    python3 tools/make_previews.py

The PPMs themselves are not committed (they are 0.2-0.9 MB each and fully
reproducible); the PNGs are, because they are the pre-photograph check of what
the screen should look like.  ramp-doubled.png is the 640x480 frame the monitor
should display; ramp-native.png is the 320x240 buffer, which is byte-comparable
with photo-display-64/tools/ramp-preview.ppm.
"""
import glob
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

for ppm in sorted(glob.glob(os.path.join(HERE, "*.ppm"))):
    png = ppm[:-4] + ".png"
    Image.open(ppm).save(png, optimize=True)
    print("wrote", os.path.basename(png), "from", os.path.basename(ppm))
