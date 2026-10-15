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

Two pages, both drawn by the display lambda in `jc4827w543.yaml`:

| page | shown when | contents |
|---|---|---|
| **BMS** | panel's own cell ≥ 10 % | LCARS console: header pills with the panel's own cell % and the clock, pack state of charge as a dial, then pack voltage, charge/discharge current and temperature as labelled data pills |
| **status** | panel's own cell < 10 % | the panel's original screen: model name, its own battery %, its own battery volts, battery icon |

The BMS page is drawn in the TNG console idiom: black field, LCARS pills (a straight
body capped by a filled circle at each end), a tan bar joining the two header pills, and
a row of coloured blocks along the bottom. Palette: `#FF9C00` orange, `#FFCC66` gold,
`#9999CC` blue, `#CC99CC` lavender, `#CC6666` salmon, `#CC9966` tan.

- The dial is a 300° ring with the gap at the bottom, drawn as 3° quads (two filled triangles
  each) — 100 of them, so one segment is exactly 1 % of state of charge. Radial spokes 1°
  apart were tried first and read as stripes on the glass; quads share their radial edges, so
  nothing shows between them and both circular edges stay smooth. The two sweep ends are
  rounded off with a filled circle each (a blunt radial cut reads as an unfinished gauge),
  and the unlit remainder sits at `#2C2C50` rather than a near-black navy so the dial still
  reads as a gauge when the state of charge is low. The arc turns salmon below
  25 % — the only place the page raises its voice. The ring carries **no** unit sign and
  nothing else inside it: at 48 pt a 3-digit reading plus `%` is wider than the inner
  diameter, and a label under the number crosses the ring's lower arms. The unit rides on
  the left-hand `PACK SOC %` label, and the two dead bands beside the dial carry that label
  and the date.
- Type sizes are named for what they are: `roboto_48` the dial number, `roboto_28` the data
  values, `roboto_24` the status page, `roboto_hdr` (20 pt) the header pills, `roboto_16`
  labels and the date. The header is deliberately the smallest type on the page — at arm's
  length it is a status strip, not a headline.
- The three data pills' outer edges line up with the header pills and the footer blocks, so
  the page has two clean columns of structure rather than three slightly different ones.
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
  layout, and reports the SOC readout's half-width against the dial's inner radius — the one
  place this design can silently collide with the ring. It is a guide, not proof: PIL's
  antialiasing and anchor points differ slightly from ESPHome's rasteriser, and it draws the
  lambda's logical canvas — the physical mounting is set separately by `rotation: 180`, which
  the preview does not model.

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

Two honest caveats left in place: the reading is load-sagged (the backlight alone moves it
~30 mV, about 5 points on the Li-ion curve), so the percentage reads a few points **low** on
battery; and the recorded history contains a two-minute 4.25 → 3.04 V excursion mid-charge that
briefly flips the label — bounded, not hidden, by the test.
