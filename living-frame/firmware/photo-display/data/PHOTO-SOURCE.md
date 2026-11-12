# photo.jpg — source, licence, exact transform

**Image:** *The Blue Marble* — the Earth seen from Apollo 17, frame
AS17-148-22727, 7 December 1972.

**Author:** NASA / Apollo 17 crew ( Harrison H. Schmitt or Ronald E. Evans ).

**Licence:** **Public domain.** NASA works are not subject to copyright in the
United States, and Wikimedia Commons records this file as
`LicenseShortName = Public domain` (verified against the Commons API on
2026-11-12). No attribution is legally required; it is recorded here anyway so
the provenance of a binary asset in this repo is never a mystery.

**Source file:** `File:The_Earth_seen_from_Apollo_17.jpg` on Wikimedia Commons —
<https://commons.wikimedia.org/wiki/File:The_Earth_seen_from_Apollo_17.jpg>
(the 960×961 copy served by `Special:FilePath?...&width=640`).

## The exact transform

The source is square; a 640×480 display needs 4:3. The crop is centred
vertically so the planet stays centred, which loses a little of the pole-to-pole
extent and no part of the disc's horizontal axis:

```bash
curl -sSL -o earth-960.jpg \
  "https://commons.wikimedia.org/wiki/Special:FilePath/The_Earth_seen_from_Apollo_17.jpg?width=640"
magick earth-960.jpg \
  -resize 640x640^ -gravity center -crop 640x480+0+0 +repage \
  -strip -quality 86 photo.jpg
```

`identify photo.jpg` → `JPEG 640x480 8-bit sRGB 108557B`.

The result is a 4:2:0 baseline JPEG at exactly the panel's native resolution, so
the decoder runs with scale factor 0 (no descaling) and no resampling happens on
the chip — the pixels that reach the frame buffer are the ones in this file,
each dithered to one bit per channel.

## Why this photo

It has the three things a 3-bit palette needs to stay recognisable: a large
saturated blue field, near-white structure (clouds) for the top end, and dark
ocean for the bottom, with a hard circular edge against black. A flat or
low-contrast photo would look like blotches at 8 colours; this one still reads
as itself, which is exactly what the acceptance photo has to show.
