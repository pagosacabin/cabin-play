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
| **BMS** | panel's own cell ≥ 10 % | pack state of charge ("%full"), pack voltage, charge/discharge current, temperature, local time + date, panel's own cell % |
| **status** | panel's own cell < 10 % | the panel's original screen: model name, its own battery %, its own battery volts, battery icon |

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
- `tools/preview.py` mirrors the lambda's geometry for a two-second look at the layout.
  It is a guide, not proof: PIL's antialiasing and anchor points differ slightly from
  ESPHome's rasteriser, and the panel runs `rotation: 180`, so on the glass the preview
  is upside down.

## Battery sensing (task 0134)

| | |
|---|---|
| Pin | `GPIO5` = ADC1_CH4 (vendor pin table: "Battery divider to ADC1_CH4") |
| Fitted divider | **160K / 220K**, node = 220/380 = 0.57895 of the pack |
| Scale factor | **1.72727** (was `1.32998`, the schematic's unfitted 33K/100K) |
| Attenuation | fixed `12db` (node peaks at 2.43 V on a full pack, so nothing clips) |
| Debug line | `[I][battery] GPIO5 (ADC1_CH4) raw=<counts> pin=<V> V batt=<V> V`, once per logged read |

The vendor reference for fitted-vs-drawn is
<https://github.com/78/xiaozhi-esp32/blob/HEAD/main/boards/guition/jc4827w543/README.md>.

A pack reading is only a pack reading with the USB cable out: while charging, the
on-board TP4056 holds the BAT net at the charge voltage (~4.2 V), which is why the
number looks flat — the reading follows the net, not the state of charge. The same
caveat applies to the 10 % page threshold: plugged in, the panel reports ~90 %.
