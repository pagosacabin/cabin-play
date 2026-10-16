# Guition JC4827W543C bench panel — firmware

> Provenance: the battery-sensing fix came from nordtronics task 0134
> (`firmware/guition-jc4827w543/`), verified 2026-10-15; scale factor
> `1.32998 -> 1.72727`. Relocated to `pagosacabin/cabin-play` per task 0136 (play
> projects live in cabin-play, never nordtronics).

The "Bench Panel 4.3" (`jc4827w543.local`, MAC `44:bd:8d:7a:cc:cc`, LAN
`192.168.1.72`) runs ESPHome, not PlatformIO.

**This directory is the panel's working copy.** The panel is flashed from a clone of
this repo on the bench host. The older standalone repo at
`~/Documents/esphome/JC4827W543/` has a configured remote
(`pagosacabin/JC4827W543-ESPHome`) that **does not exist on GitHub**, so it is a stale
copy of the config — the flashable file is the one here.

```bash
git clone git@github.com:pagosacabin/cabin-play.git
cd cabin-play/guition-jc4827w543
# stage secrets.yaml on the host first (mode 600) — it is not in the repo
esphome compile jc4827w543.yaml
esphome upload  jc4827w543.yaml --device 192.168.1.72   # OTA; no USB cable needed
esphome logs    jc4827w543.yaml --device 192.168.1.72   # stream the console
python3 tools/preview.py preview.png                    # offline render of both pages
python3 tools/audit_layout.py                           # gates the layout; non-zero if it fails
```

`secrets.yaml` (`wifi_ssid`, `wifi_password`, `ota_password`, plus a now-unused
`panel_auth_header` left over from the retired photo pipeline) is gitignored on the host
and never mirrored here; this file references those values as `!secret` only.

## Display

Two pages, both drawn by the display lambda in `jc4827w543.yaml`:

| page | shown when | contents |
|---|---|---|
| **BMS** | panel's own cell ≥ 10 % | LCARS console: one big peach elbow framing the page, the panel's own cell % / clock / date in black on the header bar, then pack state of charge as the page's biggest number on a label pill with its meter directly beneath it, and pack voltage, charge/discharge current and temperature as three equal label-pill-plus-reading columns along the bottom |
| **status** | panel's own cell < 10 % | the panel's original screen: model name, its own battery %, its own battery volts, battery icon |

The BMS page is drawn in the TNG console idiom (restyle 2026-10-16, canonical palette and
swept elbows in 0141, **geometry pass in 0144**): black field, canonical TNG palette, LCARS
pills (a straight body capped by a filled circle at each end), and **one big asymmetric elbow**
framing the page — a 27 px column down the left edge turning into the full-width header bar,
with a short periwinkle stub turning down off the bar's right end.

**Layout (0144).** Everything sits on one content margin, x = 72, which is clear of the
elbow everywhere (the swept joint lives inside the header band: it reaches x = 61 at y = 22
and x = 41 just below the bar). The page is then two bands:

```
y  8.. 34   header bar          PANEL 91 % · 11:19 · Wed 15 Oct
y 42.. 64   SOC label pill      x 72..183
y 76..135   SOC number's ink    80 pt, x 72..207
y152..172   SOC meter           x 72..471  (lavender track, orange fill)
y196..218   three label pills   x 72..197 / 208..333 / 344..469
y226..245   three readings      28 pt, one under each pill
```

Why it changed, in the order the defects were reported: the SOC pill used to start at x = 44
— 9 px off the column and inside the elbow's reach, which is what read as jammed into the
joint; the stat pills were 85×15 beside 28 pt numbers, so the pill was a shrunk chip next to
a value twice its height; and the meter sat at y = 212, 70 px below the number it belongs to,
with a row of five colour blocks under it that meant nothing. The block row is gone, the meter
moved up to sit directly under the number, and each reading is now a 126 px column holding a
23 px label pill (as tall as the reading's own ink) and the reading itself.

The elbow joint is **swept, not subtracted.** `sweep()` draws a chain of 13 px discs along a
26 px-radius arc centred on the joint, which adds the concave inner corner (radius = half the
bar width) while the outer corner stays the bars' own round cap. The previous revision instead
cut a 9 px black disc out of the corner: that is a subtraction, so it eats both bars and leaves
a round black blob — the defect this revision exists to remove. The same subtraction at the
column's foot is gone too, so the footer joint is a plain right angle. Any subtraction is also
a fill-order trick (it must come after the bars and before any text), which is exactly why a
sweep is more robust: it is drawn *with* the bars.

Palette (canonical TNG hex, revised 0141, unchanged by 0144 — the palette block is
byte-identical to the 0141 revision, `sha256 75656057…` in the lambda and `2f04a183…` in the
preview, so renders stay comparable across the two): `#FFCC99` peach (the elbow frame: column +
header bar), `#FF9900` orange (the SOC and CHARGE label pills, the meter fill), `#FFCC66` light
orange (the pack-voltage pill), `#CC6666` salmon (the meter below 25 %, and the pill turns
DISCHG/salmon on discharge), `#CC99CC` purple (the temperature pill), `#9999CC` lavender (the
meter's track), `#9999FF` periwinkle (the right stub), `#FFFF99` pale yellow (the SOC number
only — "highlights only" in the spec), white for the readings on black, black for text on any
lit bar. **There is no green and no cyan
anywhere on the page**, and no colour from the previous palette survives — `#FFFFCC` and
`#FFCC99`-as-`GOLD` are gone, and with them the old `#2C2C50` dial arc. Checked by family, not
by hex: `tools/audit_layout.py` counts zero pixels that are green-dominant or cyan-dominant.

- The SOC readout is the brightest thing on the page on purpose: at arm's length it is where
  the eye should land first, so it is the only large run of `#FFFF99`.
- The state-of-charge bar is the LCARS readout for a value with a range: a lavender track with
  an orange fill pill drawn over it (so its right end is round like every other shape), turning
  salmon below 25 % — one of the two places the page raises its voice. It spans the full content
  width, directly under the number, so the reading has a scale to sit on. At 0 % only the end
  knob is lit, and at `NAN` the track is drawn bare with no fill at all.
- The other raised voice is the current pill: `CHARGE` on orange, `DISCHG` on salmon, so the
  sign is legible without reading the number. `NAN` stays `CHARGE`-coloured with a `--` reading
  rather than guessing a direction.
- **Text y values are the ASCENDER line, not the ink.** ESPHome's `glyph_to_glyphinfo()` sets
  each glyph's `offset_y = ascender - bitmap_top` and the renderer draws from `y_start +
  offset_y`, so `TextAlign::TOP_*` anchors the ascender line: an 80 pt digit's ink starts 17 px
  below the y in the config, a 28 pt string's 6 px, a 20 pt header field's 5 px, a 16 pt pill
  label's 3 px. Every y above is chosen for the ink, not the anchor.
- Type sizes are named for what they are: `roboto_80` the SOC number (with an explicit
  `glyphs: "0123456789-."` — left to the default set, ESPHome would rasterise ~95 ASCII glyphs
  at 80 pt, roughly 200 kB of the 4 MB flash, to draw three digits), `roboto_28` the readings,
  `roboto_24` the status page, `roboto_hdr` (20 pt) the header labels, `roboto_16` the pill
  labels. The header is deliberately not the biggest type on the page — it is a status strip.
- Text printed on a pill must be given the pill's own colour as its `background`: ESPHome's
  font blends glyph edges against that colour, so the default black would ring every letter.
- The threshold is the `low_batt_pct` substitution. Below it the BMS page is replaced —
  cabin data is worthless on a panel that is about to die, and the status page says
  "charge me" in the panel's own terms.
- The four BMS values are **imported from Home Assistant** (`platform: homeassistant`,
  `sensor.jk_bms_jk_bms_{state_of_charge,total_voltage,current,temperature_sensor_1}`)
  over the native API. HA pushes the live state at connect and on every change, so the
  panel runs no polling loop of its own and stores no credential. The imports are
  `internal: true`, so they do not add entities to the panel's HA device page.
- State is `NAN` until HA's first push, and the display prints `--` for any value it
  cannot vouch for rather than a stale number. Current is signed by the BMS (charge
  positive), and its label follows the sign: `CHARGE` / `DISCHG`.
- Local time comes from HA too, so the panel inherits HA's timezone and DST rules instead
  of carrying a POSIX TZ string that has to be revisited twice a year.
- `[I][bms]` prints one line a minute (`soc= V= I= T= local=`) — the only readout of what
  the panel is actually showing, since the panel has no screenshot path.
- The random-photo idle screensaver and the `Next image` button were **removed**
  (2026-10-15, at the user's request): the panel no longer fetches images at all. The
  remaining HA entities are Display Backlight, Sleep time, Idle brightness, Wake screen.
- `tools/preview.py` mirrors the lambda's geometry and palette for a two-second look at the
  layout, and reports each reading's width against its own column and the SOC number's width
  against the content edge — the places this design can silently overflow. It is a guide, not
  proof: it draws the lambda's logical canvas, so the physical mounting (`rotation: 180`) and
  the panel's own colour polarity (`invert_colors`) are outside it.
  **It is now faithful about text, which it was not before 0144.** It drew every string with
  PIL's `'t'` anchor (the ink box) while the firmware anchors the ascender line, so the mirror
  floated each string 3–17 px above where the panel puts it — 17 px on the 80 pt number. The
  anchor is now `'a'`, which is the same model as `offset_y = ascender - bitmap_top`, so the
  same y goes into the config and into this file and the two agree to within antialiasing.
- `tools/audit_layout.py` is the check that can run without a panel: it re-derives every box
  from the same constants **and from the font metrics** and measures the rendered pixels —
  palette by hex count, the banned colour families by hue, the retired colours, a black-blob
  search inside the elbow band, the joint's monotonicity, every string's measured ink box
  against the box the constants predict, each label inside its own pill and clear of the cap
  radius, each reading inside its own column, the three columns' equal width and gutter, the
  meter's fill against 86 %, the alarm/discharge/0 %/`NAN` states, that no content sits on the
  bottom edge any more, and that the column runs the full height. 63 checks, non-zero exit on
  failure. The render it audits is the committed `preview-bms-lcars.png`.

## Battery sensing (task 0134)

| | |
|---|---|
| Pin | `GPIO5` = ADC1_CH4 (vendor pin table: "Battery divider to ADC1_CH4") |
| Fitted divider | **160K / 220K**, node = 220/380 = 0.57895 of the pack |
| Scale factor | **1.72727** (was `1.32998`, the schematic's unfitted 33K/100K) |
| Attenuation | fixed `12db` (node peaks at 2.43 V on a full pack, so nothing clips) |
| Debug line | `[I][battery] GPIO5 (ADC1_CH4) raw=<counts> pin=<V> V batt=<V> V state=CHG\|BATT`, once per logged read |

The vendor reference for fitted-vs-drawn is
<https://github.com/78/xiaozhi-esp32/blob/HEAD/main/boards/guition/jc4827w543/README.md>.

## Charging state — inferred, because the board cannot report it

The reading is only a state of charge with the USB cable **out**: while charging, the on-board
TP4056 parks the BAT net on its CV plateau, so the number follows the charger rather than the
cell. The vendor's notes give the hardware reason it cannot be read directly — the charger's
status leg is not connected to the ESP32.

So the page infers it from the reading itself, and stops printing a percentage it cannot stand
behind: the header pill reads `PANEL CHG` instead. Exported to Home Assistant as
`sensor.living_room_bench_panel_4_3_panel_battery_state` (`CHG` / `BATT` / `--`).

| | |
|---|---|
| Enter `CHG` | reading ≥ **4.20 V** for 2 consecutive polls (the TP4056's CV setpoint — above anything a resting cell reaches on this board: 4.16 V unloaded, ~4.05 V with the backlight on) |
| Leave `CHG` | reading < **4.10 V** for 2 consecutive polls |
| Poll | 30 s |

**Scope, measured rather than assumed.** The plateau is what this sees. A charger in *bulk*
drives the net from the cell's own voltage, so the label reads `BATT` and the percentage climbs
through real values — the mask is not needed there. A trend-based version (rising/falling
evaluated per poll) was tried first and discarded: ±3 mV between 30 s polls sits under this
reading's own noise (±15 mV), and it flapped CHG/BATT ~50 times across a single discharge.

`tools/test_batt_state.py` replays the rule against the panel's real 24 h history plus synthetic
bulk-charge, load-step, resting-cell, unplug and boot cases, and fails on flapping. Run it after
touching either threshold.

Two honest caveats left in place: the reading is load-sagged (measured: the backlight alone
moves it 39 mV, about 6.5 points on the Li-ion curve) and only the *load-proportional* part of
that is compensated — see the next section; and the recorded history contains a two-minute
4.25 → 3.04 V excursion mid-charge that briefly flips the label — bounded, not hidden, by the
test.

## Load-sag compensation

The SoC curve is a *resting*-voltage curve, but the reading is the cell's terminal voltage under
this panel's own load, so the percentage sits low. The backlight is the largest single load and
its duty is known to the firmware, so that part of the sag is added back before the curve lookup:

```
V_rest ≈ V_measured + batt_sag_v_per_duty × duty      (duty: 0 = off, 1 = full)
```

| | |
|---|---|
| Constant | `batt_sag_v_per_duty`, in `substitutions:` — measured **0.0392** on this board |
| Measured by | `tools/calibrate_batt_sag.py` |
| Applied | in the `batt_pct` lambda, and only when **not** charging |

On this board the measured result was **39 mV at full duty** (100 %: 3.8478 V bracketed, 0 %:
3.8869 V), which is ~6.5 points of state of charge.

**The measurement is slow on purpose (25 min).** A Li-ion cell does not step to a new voltage
when its load changes — it relaxes over minutes — so a short settle measures the transient rather
than the sag. The first version of the tool used 60 s settles and reported 0 % and 50 % duty
within 1.4 mV of each other, with 26 mV of movement still inside its own sampling window. On top
of that the panel drains its cell at a few mV/min, which across one phase is comparable to the
sag being measured; so the tool holds full duty *either side* of the unloaded phase and averages
them, which makes the cell's own discharge common to both and cancels it. On this board that
drift measured **40.9 mV across the run — larger than the 39 mV sag being measured**, so the
bracketing is not a refinement here, it is the difference between a number and a coincidence.

**Not compensated:** the constant part of the sag — ESP32, WiFi, DC-DC, none of which switch off
— cannot be separated from the cell's true resting voltage by this ADC alone; only a multimeter
at rest can tell them apart. Expect the reading to stay a point or two low. Linearity in duty is
assumed: true for PWM'd LED current, and the only duties this panel uses in anger are 1.0 and the
idle dim level.
