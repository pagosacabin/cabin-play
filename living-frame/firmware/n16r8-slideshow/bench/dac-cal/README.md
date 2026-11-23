# DAC calibration — test frames and the bench procedure (task 0250, rung 2)

Twelve solid frames and the steps that turn them into the per-channel voltage
table that rung 3 needs. Rung 1 (this directory and the code restructure) needs
no bench hands; **rung 2 does** — a DC multimeter on the VGA connector with the
monitor connected.

The frames were generated with `tools/make_dac_frames.py`; the SHA-256 manifest
in `test-frames/SHA256SUMS` is what the card's copy is checked against.

## What is here

Paths are relative to `living-frame/firmware/n16r8-slideshow/`:

```
bench/dac-cal/test-frames/DAC-R0.BIN … DAC-B3.BIN   12 files, 480,000 B each (800x600, 1 byte/pixel)
bench/dac-cal/test-frames/SHA256SUMS                sha256 of each file
tools/make_dac_frames.py                            generator (deterministic)
```

Regenerate and re-verify at any time; the digests are stable:

```bash
/usr/bin/python3 tools/make_dac_frames.py --out bench/dac-cal/test-frames
cd bench/dac-cal/test-frames && sha256sum -c SHA256SUMS
```

The steps below assume `cd living-frame/firmware/n16r8-slideshow` first.

Each file is a **solid** frame: one DAC byte repeated 480,000 times, in the
accepted packed format `(B<<4)|(G<<2)|R`. The slideshow streams the file straight
into the framebuffer (`memcpy` in `bounce_fill`, no LUT, no dither), so the byte
on the wire at the VGA pin is literally the byte in the file:

| file | byte | R code | G code | B code |
|---|---|---|---|---|
| `DAC-R0.BIN`, `DAC-G0.BIN`, `DAC-B0.BIN` | `0x00` | 0 | 0 | 0 |
| `DAC-R1.BIN` | `0x01` | 1 | 0 | 0 |
| `DAC-R2.BIN` | `0x02` | 2 | 0 | 0 |
| `DAC-R3.BIN` | `0x03` | 3 | 0 | 0 |
| `DAC-G1.BIN` | `0x04` | 0 | 1 | 0 |
| `DAC-G2.BIN` | `0x08` | 0 | 2 | 0 |
| `DAC-G3.BIN` | `0x0c` | 0 | 3 | 0 |
| `DAC-B1.BIN` | `0x10` | 0 | 0 | 1 |
| `DAC-B2.BIN` | `0x20` | 0 | 0 | 2 |
| `DAC-B3.BIN` | `0x30` | 0 | 0 | 3 |

The three code-0 frames are the same black file by construction — they are the
meter's own zero check (all three channels should read the same ~0 V), not three
different pictures.

Displayed by **`n16r8-slideshow`**, which enumerates `/sdcard/FRAMES` sorted by
name and shows each frame for ~10 s, looping. Name order is R0,R1,R2,R3,G0…B3,
which is the measurement order. A full pass is about 2 minutes.

## Step 1 — back up the card before it is touched

The card holds the LIVE library's source frames and 0241's `/NEWCARD.BIN`
evidence. They are **not** modified by this procedure, but the `/sdcard/FRAMES`
set is temporarily moved aside, so back it up first and restore it after.

```bash
# card in the laptop reader (it is NOT in the board's holder for this)
udisksctl mount -b /dev/sdX1                       # or mount it by label
MNT=/run/media/$USER/<VOLUME>
BK=~/frames-backup-0250-$(date +%Y%m%d-%H%M)
mkdir -p "$BK"
cp -a "$MNT/FRAMES" "$BK/FRAMES"
find "$BK/FRAMES" -type f | wc -l                  # record the count
( cd "$BK/FRAMES" && sha256sum * > "$BK/FRAMES.sha256" )
```

Record: the file count and the presence of `FRAMES.sha256`. `/NEWCARD.BIN` and
`/LIVE` (if present) are left where they are and are not read by this procedure.

## Step 2 — put the twelve test frames on the card

```bash
mkdir -p "$MNT/FRAMES-CABIN"                       # the originals, set aside
mv "$MNT/FRAMES/"*.BIN "$MNT/FRAMES-CABIN"/ 2>/dev/null || true
cp test-frames/DAC-*.BIN "$MNT/FRAMES/"
SUMS=$PWD/test-frames/SHA256SUMS
( cd "$MNT/FRAMES" && sha256sum -c "$SUMS" )       # 12 x OK
ls "$MNT/FRAMES"                                   # exactly 12 .BIN
sync && udisksctl unmount -b /dev/sdX1
```

`FRAMES` must hold **only** the twelve files: the slideshow cycles every `.BIN`
it finds, so a leftover photo would land between readings.

## Step 3 — flash the slideshow, connect the monitor

```bash
cd living-frame/firmware/n16r8-slideshow
pio run -e n16r8-slideshow -t upload               # FT232R bridge, /dev/ttyUSB0
```

Card back in the board, monitor on the VGA port, then confirm on screen that the
first frame is black and the second is dark red (`DAC-R1`). The console on this
bench is unusable (0225) — the screen is the only indicator, and that is enough
here: each frame is a flat colour.

## Step 4 — measure

Probe the VGA connector (DB15) **with the monitor connected** — its 75 Ω
termination is part of the divider:

| pin | signal |
|---|---|
| 1 | R |
| 2 | G |
| 3 | B |
| 5, 6, 7, 8, 10 | GND (use 5 or 10 as the reference clip) |

A DC meter averages over blanking, which scales every reading on a channel by the
same active-frame fraction. The table only needs the ratios `V[v]/V[3]` within a
channel, so that common scale cancels and no correction is needed.

Take **two readings per cell**, on separate passes if convenient (the frames come
round every ~2 minutes). Record the frame name with each pair so a transposed row
is visible later.

| frame | R (V) #1 | #2 | G (V) #1 | #2 | B (V) #1 | #2 |
|---|---|---|---|---|---|---|
| `DAC-?0` (black, zero check) | | | | | | |
| `DAC-?1` | | | | | | |
| `DAC-?2` | | | | | | |
| `DAC-?3` (full scale) | | | | | | |

Then state **full scale per channel**: `V_R[3]`, `V_G[3]`, `V_B[3]` in volts.
A useful sanity figure is the ideal ratio `1/3` and `2/3` against the reading —
the rung-3 tables need only the ratios, but absolute full scale is what says
whether the ladder is where it was designed to be.

Nothing is estimated: a cell that cannot be read is recorded as unread and the
table is reported incomplete.

## Step 5 — restore the card

```bash
udisksctl mount -b /dev/sdX1
rm "$MNT/FRAMES/"DAC-*.BIN
mv "$MNT/FRAMES-CABIN/"*.BIN "$MNT/FRAMES/"
rmdir "$MNT/FRAMES-CABIN"
find "$MNT/FRAMES" -type f | wc -l                 # equals the Step 1 count
( cd "$MNT/FRAMES" && sha256sum -c "$BK/FRAMES.sha256" )
sync && udisksctl unmount -b /dev/sdX1
```

Both the count and the hash list must match Step 1 exactly. Then reflash
whichever build was on the board before this session — the camera side is
`pio run -e freenove-cam -t upload` in `living-frame/firmware/freenove-cam`, the
frame side `pio run -e n16r8-photo -t upload` in `living-frame/firmware/n16r8-live`
— and leave the hardware as it was found.

## Step 6 — what rung 3 consumes

The 3×4 voltage table plus full scale per channel. Rung 3 writes
`V[v]/V[3]` into `s_dac_v[3][4]` in
`living-frame/firmware/freenove-cam/src/main.cpp` and rebuilds; the levels are
linearised with `DAC_GAMMA` (2.2) when the quant table is built, so no other
number changes.
