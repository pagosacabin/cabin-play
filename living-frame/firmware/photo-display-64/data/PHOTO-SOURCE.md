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

---

# parrot.jpg — source, licence, exact transform (round 2)

**Image:** *Wide angle rainbow lorikeet* (Trichoglossus moluccanus),
photographed at a wide angle.

**Author:** Dimitrije Matovic.

**Licence:** **CC BY 4.0** — <https://creativecommons.org/licenses/by/4.0/>.
Attribution is required: *"Wide angle rainbow lorikeet" by Dimitrije Matovic,
licensed CC BY 4.0, via Wikimedia Commons.* No ShareAlike condition applies, so
the down-scaled 640×480 copy below carries the same licence with the same
attribution; it is a format/size transform, not a new work.

**Source file:** `File:Wide_angle_rainbow_lorikeet.jpg` on Wikimedia Commons —
<https://commons.wikimedia.org/wiki/File:Wide_angle_rainbow_lorikeet.jpg>
(5184×3456, `LicenseShortName = CC BY 4.0`, read from the Commons API on
2026-11-12).

## The exact transform

4:3 crop, centred, then a plain resize to the panel's native 320×240 doubled
(640×480), so TJpgDec's 1/2 descale lands on exactly 320×240 with no rescale:

```bash
curl -sL -A 'living-frame-bench/1.0' -o lorikeet.jpg \
  "https://upload.wikimedia.org/wikipedia/commons/.../Wide_angle_rainbow_lorikeet.jpg"
magick lorikeet.jpg -auto-orient -resize 640x480^ -gravity center \
  -extent 640x480 -strip -quality 92 parrot.jpg
```

`identify parrot.jpg` → `JPEG 640x480 8-bit sRGB 99010B`,
sha256 `4eeed2ba3e7126963d93ce00f34586499488fdd872853c44fc86abbfcafc5415`.

## Why this photo

The rejection was about the picture reading as a dot puzzle on a near-neutral
subject, so round 2 needed a subject that actually exercises a 64-colour
palette. Candidates were measured by running each through this project's own
pipeline (`tools/render_check.cpp`) and counting the palette entries the frame
really uses, plus mean saturation of the 320×240 frame:

| candidate | palette used | saturated pixels | mean saturation |
|---|---|---|---|
| Blue Marble (round 1) | 24/64 | 12 % | 0.16 |
| scarlet macaw (zoo close-up) | 36/64 | 12 % | 0.16 |
| scarlet macaw (CC0) | 31/64 | 34 % | 0.28 |
| autumn foliage (CC0) | 42/64 | 97 % | 0.80 |
| **rainbow lorikeet (chosen)** | **45/64** | 68 % | 0.42 |

The lorikeet wins on the metric that matters here — it is the only candidate
that addresses 45 of the 64 entries — and it is a parrot, which is what the
task suggested. Its light-domain block error is also the lowest of the set
(1.67/1.53/1.29 of 255), i.e. the colours it does use are reproduced
accurately.

