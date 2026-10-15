# Guition JC4827W543C bench panel — firmware (mirror)

> Provenance: mirrored from nordtronics task 0134 (`firmware/guition-jc4827w543/`),
> verified 2026-10-15; battery scale-factor fix `1.32998 -> 1.72727`. Relocated to
> `pagosacabin/cabin-play` per task 0136 (play projects live in cabin-play, never nordtronics).

The "Bench Panel 4.3" (`jc4827w543.local`, MAC `44:bd:8d:7a:cc:cc`) runs ESPHome,
not PlatformIO. Its configuration lives outside this repo, at
`~/Documents/esphome/JC4827W543/` on the bench host, which is a standalone git
repo whose configured remote (`pagosacabin/JC4827W543-ESPHome`) **does not exist
on GitHub** — so until that repo is created there is no origin to point at.

`jc4827w543.yaml` here is a **byte-identical mirror** of the file flashed to the
panel, so a change to the panel's battery sensing is reviewable in this repo.
sha256 of the mirrored copy:

```
d355ded833659ec972442612df107e42b8f978e49438c037226e607083d965cc  jc4827w543.yaml
```

Flashing still happens from the bench host, and `secrets.yaml` (Wi-Fi, OTA
password, panel Basic-auth header) is gitignored there and never mirrored here —
this file references those values as `!secret` only.

## Why it is in this repo at all

Mailbox task 0134 asked for "the Guition firmware" fix behind a branch + SHA on
origin. The panel is part of this project's bench fleet (its idle photo is served
by the project's VPS), but no part of its firmware was under version control in a
reachable origin, so the deliverable could not be referenced by a branch tip. The
mirror closes that gap.

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
number looks flat — the reading follows the net, not the state of charge.
