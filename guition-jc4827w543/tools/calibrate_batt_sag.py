#!/usr/bin/env python3
"""Measure how much the panel's own load sags its battery reading, and fit the compensation.

The reading is the cell's terminal voltage *under the panel's own load*, but the SoC curve in
jc4827w543.yaml is a *resting*-voltage curve, so the percentage sits low by however much the
panel sags its own cell. The backlight is the largest single load and its duty is known to the
firmware, so that part of the sag can be measured and added back:

    V_rest ~= V_measured + K * duty

Why this is a slow, three-phase measurement rather than a quick sweep:

  * **Relaxation.** A Li-ion cell does not step to a new voltage when its load changes; it
    relaxes over minutes (diffusion-limited). A 60 s settle measures the transient, not the
    sag -- the first version of this tool did that and reported 0 % and 50 % duty within
    1.4 mV of each other, with a 26 mV spread still moving inside the sampling window.
  * **Drift.** The panel is a heavy load for a small cell: it was draining ~2.4 mV/min during
    the run, which over a 10 minute phase is comparable to the sag being measured. So the
    full-duty phase is measured *either side* of the unloaded one and averaged: the cell's own
    discharge is then common to both and cancels, leaving the load step.

Phase 1 needs no wait because the panel is already sitting at full brightness.
Total runtime is about 2 * (--settle) + 4 minutes.

What it deliberately does not do is separate the *constant* part of the sag (ESP32 + WiFi,
which never turns off) from the cell's true resting voltage: from the ADC alone the two are
indistinguishable, and only a multimeter at rest can tell them apart (see README, battery
sensing). So this fits the load-proportional term, which is the identifiable one, and assumes
the response is linear in duty -- true for PWM'd LED current, and the only duty values this
panel actually uses are 1.0 (active) and the idle dim level.

Usage:
    python3 tools/calibrate_batt_sag.py [--settle 600] [--dry-run]
"""
import argparse
import json
import re
import statistics
import sys
import time
import urllib.request

VOLT_ENTITY = 'sensor.bench_panel_4_3_battery_voltage'
BACKLIGHT = 'light.bench_panel_4_3_display_backlight'
SWEEP = [100, 0, 100]        # % duty; the two full-brightness phases bracket the unloaded one
STEADY_SPREAD = 0.008        # V: in-window spread above which a phase is called not settled


def ha():
    env = dict(re.findall(r'^([A-Z_]+)=(.*)$', open('/home/astroboy/.hermes/.env').read(), re.M))
    url = env.get('HASS_URL', 'http://192.168.1.103:8123').strip('"').rstrip('/')
    return url, {'Authorization': 'Bearer ' + env['HASS_TOKEN'].strip('"'),
                 'Content-Type': 'application/json'}


def get(url, hdr, path):
    return json.load(urllib.request.urlopen(urllib.request.Request(url + path, headers=hdr),
                                            timeout=25))


def set_backlight(url, hdr, pct):
    if pct == 0:
        body, path = {'entity_id': BACKLIGHT}, '/api/services/light/turn_off'
    else:
        body = {'entity_id': BACKLIGHT, 'brightness_pct': pct}
        path = '/api/services/light/turn_on'
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), headers=hdr,
                                 method='POST')
    urllib.request.urlopen(req, timeout=25).read()


def current_brightness(url, hdr):
    s = get(url, hdr, f'/api/states/{BACKLIGHT}')
    return 0 if s['state'] != 'on' else round(100 * (s['attributes'].get('brightness') or 0) / 255)


def sample(url, hdr, n=9, gap=10):
    """Median of the last n readings, plus the spread across them (a settling check)."""
    vals = []
    for i in range(n):
        try:
            vals.append(float(get(url, hdr, f'/api/states/{VOLT_ENTITY}')['state']))
        except (ValueError, KeyError):
            pass
        if i < n - 1:
            time.sleep(gap)
    if not vals:
        return None, None
    return statistics.median(vals), max(vals) - min(vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--settle', type=int, default=600,
                    help='seconds to hold each changed phase before sampling (default 600)')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    if args.dry_run:
        print('dry run: nothing measured')
        return 0

    url, hdr = ha()
    start_duty = current_brightness(url, hdr)
    print('backlight starts at %d%% (will be restored)' % start_duty)
    print('phases %s %% duty, %d s settle where the duty changes (~%d min total)'
          % (SWEEP, args.settle, (2 * args.settle + 240) // 60))

    rows = []
    try:
        for pct in SWEEP:
            already = current_brightness(url, hdr) == pct
            if already:
                print('  %3d%%: already there, sampling without waiting' % pct)
            else:
                set_backlight(url, hdr, pct)
                print('  %3d%%: set, settling %d s ...' % (pct, args.settle))
                time.sleep(args.settle)
            med, spread = sample(url, hdr)
            if med is None or spread is None:
                print('       no reading')
                continue
            flag = '' if spread < STEADY_SPREAD else '  <- still moving, treat as unsettled'
            print('       %.4f V (spread %.1f mV over 90 s)%s' % (med, spread * 1000, flag))
            rows.append((pct / 100.0, med))
    finally:
        set_backlight(url, hdr, start_duty or 100)
        print('backlight restored to %d%%' % (start_duty or 100))

    by_duty = {}
    for duty, v in rows:
        by_duty.setdefault(duty, []).append(v)
    full = by_duty.get(1.0, [])
    off = by_duty.get(0.0, [])
    if not full or not off:
        print('\nneed both a full-duty and an unloaded phase to fit')
        return 1

    v_full = statistics.mean(full)
    drift = (max(full) - min(full)) if len(full) > 1 else 0.0
    k = statistics.mean(off) - v_full
    print('\n  full duty (bracketing mean)  %.4f V' % v_full)
    print('  unloaded                     %.4f V' % statistics.mean(off))
    print('  cell drift across the run    %.1f mV (between the two full-duty phases)'
          % (drift * 1000))
    print('  => sag at full duty          %.0f mV' % (k * 1000))
    print('  => on the 3.6-4.2 V window   ~%.1f points of state of charge'
          % (100 * k / 0.6))
    print('\npaste into jc4827w543.yaml substitutions:')
    print('  batt_sag_v_per_duty: "%.4f"' % k)
    if k <= 0:
        print('\nWARNING: no sag resolved. Is the panel on the charger? (Then the net is held '
              'by the TP4056 and the load response is hidden.)')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
