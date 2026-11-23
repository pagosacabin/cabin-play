#!/usr/bin/env python3
"""0243: download the ten bench frames from Wikimedia Commons.

These are *bench stand-in* photographs -- Stephen curates the real ones later.
Every file below is either public domain or CC BY / CC BY-SA, the licence and
author are recorded in sources.json next to the downloads, and the download is
the 1800 px-wide render of the Commons original (the converter resamples to
600x800 portrait and rotates, so 1800 px is plenty).

    ./fetch_frames.py --out frames-src

Files are addressed by exact Commons title, not by search, so a re-run
downloads the same ten pictures and the frame hashes stay reproducible.
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

UA = {"User-Agent": "nordtronics-bench/1.0 (nordlus@gmail.com)"}

# (slot, file name, licence)
FILES = [
    (1,  "Elephant bull in green savanna ... (32888320068).jpg",            "CC BY-SA 2.0", "Bernard DUPONT"),
    (2,  "Glencoe Lochan reflections 3 20211022.jpg",                       "CC BY-SA 3.0", "Daniel Kraft"),
    (3,  "Panorama della Val d'Orcia da Via del Casello, Pienza.jpg",       "CC BY 4.0",    "Travel Designer"),
    (4,  "Beech Forest (AU), Great Otway National Park, Beauchamp Falls -- 2019 -- 1271.jpg",
                                                                            "CC BY-SA 4.0", "Dietmar Rabich"),
    (5,  "001 Humpback whale breaching in Ballena Marine National Park Photo by Giles Laurent.jpg",
                                                                            "CC BY-SA 4.0", "Giles Laurent"),
    (6,  "Oia, Santorini (2600397727).jpg",                                 "CC BY-SA 2.0", "bongo vongo"),
    (7,  "171017-FS-Superior-SAR-fallhamlakefirearea4 (37812505281).jpg",   "Public domain","Superior National Forest (USFS)"),
    (8,  "A peacock displaying it's covert feathers.jpg",                   "CC BY-SA 4.0", "Chinmayee Mishra"),
    (9,  "054a Dead camel thorn trees in Deadvlei Photo by Giles Laurent.jpg",
                                                                            "CC BY-SA 4.0", "Giles Laurent"),
    (10, "Arabian Red Fox 3.jpg",                                           "CC BY-SA 4.0", "Commons Quality image"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="frames-src")
    ap.add_argument("--width", default="1800")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    manifest = []
    for slot, name, lic, author in FILES:
        url = ("https://commons.wikimedia.org/wiki/Special:FilePath/"
               + urllib.parse.quote(name.replace(" ", "_")) + "?width=" + args.width)
        dest = os.path.join(args.out, "src%02d.jpg" % slot)
        req = urllib.request.Request(url, headers=UA)
        with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
            f.write(r.read())
        size = os.path.getsize(dest)
        print("%2d %8d B  %-12s %s" % (slot, size, lic, name))
        manifest.append({
            "slot": slot, "file": dest, "bytes": size, "license": lic, "author": author,
            "title": "File:" + name,
            "page": "https://commons.wikimedia.org/wiki/File:" + name.replace(" ", "_"),
        })
    with open(os.path.join(args.out, "sources.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("sources written to", os.path.join(args.out, "sources.json"))


if __name__ == "__main__":
    sys.exit(main())
