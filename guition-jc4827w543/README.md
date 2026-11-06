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
```

`secrets.yaml` (`wifi_ssid`, `wifi_password`, `ota_password`, plus a now-unused
`panel_auth_header` left over from the retired photo pipeline) is gitignored on the host
and never mirrored here; this file references those values as `!secret` only.

## Display

Three pages, all drawn by the display lambda in `jc4827w543.yaml`:

| page | shown when | contents |
|---|---|---|
| **BMS** | panel's own cell ≥ 10 %, no proximity contact | LCARS console: one big peach elbow framing the page, the panel's own cell % / clock / date in black on the header bar, a purple rule, then pack state of charge as the page's biggest number over a two-tone bar and pack voltage, charge/discharge current and temperature as label pills with large white readings |
| **status** | panel's own cell < 10 % | the panel's original screen: model name, its own battery %, its own battery volts, battery icon |
| **tracker** | the C4002 reports presence, for 20 s after it clears, or while the *Tracker page* switch pins it | proximity scope: a 120° wedge (the sensor's own field of view) with a sweeping arm that ping-pongs inside it, the blip on that arm at a radius the single-target range sets, `NEAREST` in metres as the big number, a `STATE` row (NONE / STILL / MOVING), and two lines that say what the part cannot do |

The BMS page is drawn in the TNG console idiom (restyle 2026-10-16, revised for canonical
palette and elbows in 0141): black field, canonical TNG palette, LCARS pills (a straight body
capped by a filled circle at each end), a row of coloured blocks along the bottom, and **one
big asymmetric elbow** framing the page — a 27 px column down the left edge turning into the
full-width header bar, with a short periwinkle stub turning down off the bar's right end.

The elbow joint is **swept, not subtracted.** `sweep()` draws a chain of 13 px discs along a
26 px-radius arc centred on the joint, which adds the concave inner corner (radius = half the
bar width) while the outer corner stays the bars' own round cap. The previous revision instead
cut a 9 px black disc out of the corner: that is a subtraction, so it eats both bars and leaves
a round black blob — the defect this revision exists to remove. The same subtraction at the
column's foot is gone too, so the footer joint is a plain right angle. Any subtraction is also
a fill-order trick (it must come after the bars and before any text), which is exactly why a
sweep is more robust: it is drawn *with* the bars.

Palette (canonical TNG hex, revised 0141): `#FFCC99` peach (the elbow frame: column + header
bar, and the second footer block), `#FF9900` orange (the main label pills, the meter fill, a
footer block), `#FFCC66` light orange (the pack-voltage pill), `#CC6666` salmon (the meter
below 25 %, a footer block), `#CC99CC` purple (the rule under the header, the temperature pill,
a footer block), `#9999CC` lavender (the meter's track, a footer block), `#9999FF` periwinkle
(the right stub), `#FFFF99` pale yellow (the SOC number only — "highlights only" in the spec),
white for the readings on black, black for text on any lit bar. **There is no green and no cyan
anywhere on the page**, and no colour from the previous palette survives — `#FFFFCC` and
`#FFCC99`-as-`GOLD` are gone, and with them the old `#2C2C50` dial arc. Checked by family, not
by hex: `tools/audit_layout.py` counts zero pixels that are green-dominant or cyan-dominant.

- The SOC readout is the brightest thing on the page on purpose: at arm's length it is where
  the eye should land first, so it is the only large run of `#FFFF99`.
- The state-of-charge bar is the LCARS readout for a value with a range: a lavender track with
  an orange fill pill drawn over it (so its right end is round like every other bar), turning
  salmon below 25 % — the only place the page raises its voice. At 0 % only the end knob is lit,
  and at `NAN` the track is drawn bare with no fill at all.
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
  layout, and reports each reading's width against the right margin and the SOC number's width
  against the block it has to fit in — the two places this design can silently overflow its
  column. It is a guide, not proof: PIL's antialiasing and anchor points differ slightly from
  ESPHome's rasteriser, and it draws the lambda's logical canvas — the physical mounting is set
  separately by `rotation: 180`, which the preview does not model, as is the panel's own colour
  polarity (`invert_colors` in the display block).
- `tools/audit_layout.py` is the check that can run without a panel: it re-derives every box
  from the same constants and measures the rendered pixels — palette by hex count, the banned
  colour families by hue, a black-blob search inside the elbow band, the joint's monotonicity,
  every text box against its own column, and the meter, alarm, 0 % and `NAN` states. 42 checks,
  non-zero exit on failure. The render it audits is the committed `preview-bms-lcars.png`.

## Proximity page — the C4002 tracker (2026-11-06)

The DFRobot C4002 24 GHz presence sensor that nordtronics task 0169 brought up on the bench is
now on this panel. The sensor board runs ESPHome on its own ESP32-C3 and is integrated in Home
Assistant as *C4002 Presence*; the panel imports four of its entities with
`platform: homeassistant`, exactly the way it already imports the JK BMS — no second radio, no
credential of its own, no polling loop. HA pushes on connect and on every change.

| panel id | HA entity |
|---|---|
| `presence` | `binary_sensor.c4002_presence_presence` |
| `tgt_dist` | `sensor.c4002_presence_range` |
| `tgt_state` | `sensor.c4002_presence_detected` (0 none / 1 still / 2 moving) |

The speed, energy and gate-mask entities were dropped from the sensor on 2026-11-06, and the page
followed: with two or more bodies in the beam those channels are not attributable to a target, so
putting them on screen implied a tracker this part is not. What is left says so out loud —
`SINGLE TARGET RANGE` / `2+ BODIES = PRESENCE ONLY` — across the space the energy bar used to fill.

Page order is **fallback → tracker → BMS**: a panel that is about to die still says "charge me"
first, whatever is walking past it. A contact sets `tracker_hold` to 20 s, so the page survives
the sensor's own hold and then a tail, and then yields back to the BMS page. The *Tracker page*
switch pins the page on regardless of contact, for inspecting it with nobody in front of the
sensor; it is also the switch Home Assistant can flip.

The sweep is animated by a 150 ms `interval` (12° per tick, so the arm crosses the wedge in 1.5 s
each way and ping-pongs rather than wrapping) and only runs while the tracker page is the one on
screen — the BMS and status pages keep their event-driven repaints, so an idle panel pays nothing
for this.

Three deliberate choices in the drawing:

- **The scope is a 120° wedge, not a circle.** That is the C4002's own field of view: the sensor
  sits at the apex (the small peach block), the two peach rays are its limits, and the arm only
  ever runs between them. A full-circle scope would be drawing coverage the part does not have.
  There is no arc primitive in ESPHome's display API, so each ring is a chain of 3° line
  segments, the same trick the console page uses for its elbow.
- **The blip has no bearing, and does not pretend to have one.** A single-range mmWave sensor
  reports distance, not angle, so the blip is painted where the arm currently is at a radius the
  range sets (scope scaled 0–6 m). Parking it at a fixed bearing would be a lie drawn in pixels.
- **No green.** A contact is amber/salmon in this palette, like every other raised voice on the
  panel. Green would be the one colour the TNG idiom does not have.

`tools/preview.py` renders the page offline next to the BMS and status pages (the last two rows of
`preview-tracker.png`); like the rest of that file it is a PIL mirror of the lambda, so it is
layout evidence, not a photograph of the panel.

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
