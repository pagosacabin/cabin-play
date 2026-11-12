# TJpgDec — provenance and changes

Vendored copy of **TJpgDec R0.03**, the tiny JPEG decompressor by ChaN
(elm-chan.org/fsw/tjpgd). Files:

| File | Origin |
|---|---|
| `tjpgd.c` | unmodified, as distributed in the R0.03 source drop |
| `tjpgd.h` | unmodified |
| `tjpgdcnf.h` | R0.03 configuration, **with one change** (below) |

The copies here were taken from the `bodmer/TJpg_Decoder` mirror of the R0.03
sources (`src/tjpgd.c`, `src/tjpgd.h`, `src/tjpgdcnf.h` at
`raw.githubusercontent.com/bodmer/TJpg_Decoder/master/src/`) because
elm-chan.org's own archive URL was returning 404 at the time of writing. The
three files are the upstream R0.03 content; only `tjpgdcnf.h` differs from it.

## Licence

TJpgDec is by ChaN and is distributed under a permissive two-clause BSD-style
licence: redistribution in source and binary form is allowed provided the
copyright notice and the conditions are retained, and the author's name is not
used to endorse derived products. The copyright header is intact at the top of
`tjpgd.c` and `tjpgd.h`. No separate licence file ships with R0.03.

## The one configuration change

```c
#define JD_FORMAT   0   // was 1 (RGB565); 0 = RGB888
```

The 3-bit VGA writer has to make an independent 1-bit decision per colour
channel, and it does so with an ordered dither that compares each 8-bit channel
against a position-dependent threshold. At `JD_FORMAT 1` the decoder has already
truncated green to 6 bits and red/blue to 5, so the dither would be operating on
values the palette never asked for. RGB888 costs 3 bytes per pixel in the MCU
output buffer (a 16x16 block = 768 bytes, on the stack of the output callback,
not a whole-image buffer) and gives the dither the full range to work with.

`JD_SZBUF 512`, `JD_USE_SCALE 1` and `JD_FASTDECODE 1` are left at upstream's
values; this project calls `jd_decomp` with scale 0 (no descaling).
