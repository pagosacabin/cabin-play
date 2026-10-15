#!/usr/bin/env python3
"""Replay jc4827w543's charging inference against real and synthetic battery traces.

The board wires no charger-status pin, so "CHG" is inferred from the reading itself (see the
text_sensor `batt_state` in jc4827w543.yaml: enters on the charger's CV plateau at >= 4.20 V,
leaves below 4.10 V, two agreeing polls per edge). That rule cannot be tested by unplugging a
cable on demand, so it is tested here: the real 24 h history of
sensor.bench_panel_4_3_battery_voltage is pushed through the same state machine, followed by
synthetic cases for what the history cannot prove.

Three checks exist because earlier versions failed them:

  * `transitions are bounded` -- the first implementation triggered on the *trend* (+-3 mV per
    30 s poll). That is under this reading's noise (+-15 mV poll to poll), so it flapped
    CHG/BATT about 50 times across one discharge. Any trend-based rewrite must pass this.
  * `a full cell at rest is not charging` -- the entry threshold has to sit above a resting
    cell's voltage, or a charged panel reports CHG with no cable attached. 4.20 V clears the
    worst measured case (4.16 V resting, 4.187 V with the backlight switched off).
  * `steady low readings are BATT` -- with a debounce the state necessarily lingers one poll
    past a dropout, so that is asserted over runs of polls, not per sample.

This is a mirror of the lambda, not a wrapper around it: change the thresholds or transitions
in the YAML, change them here too, and re-run.
"""
import datetime as dt
import json
import random
import re
import sys
import urllib.request

HIST = 'sensor.bench_panel_4_3_battery_voltage'
POLL_S = 30            # the text_sensor's update_interval
ENTER = 4.20           # V: the charger's CV plateau (the TP4056's setpoint)
LEAVE = 4.10           # V: the hysteresis floor
DEBOUNCE = 2           # agreeing polls needed per edge (60 s)
LOW_V = 3.9            # V: readings this low can never be a charger
# 24 h of real switching is BATT -> CHG -> BATT, i.e. 2 changes; the recorded trace also holds
# a two-minute 4.25 -> 3.04 V excursion mid-charge, worth one extra pair, i.e. 4. More than 5
# means flapping; the first (trend-based) implementation scored ~100.
MAX_TRANSITIONS = 5
MIN_CHG_COVERAGE = 0.90  # fraction of the first..last CHG span actually spent in CHG


def state_machine(series):
    """series: [(t, volts)] -> [(t, volts, charging)], exactly as the lambda decides it."""
    charging, agree, out = False, 0, []
    for t, v in series:
        want = (v < LEAVE) if charging else (v >= ENTER)
        agree = agree + 1 if want else 0
        if agree >= DEBOUNCE:
            charging, agree = not charging, 0
        out.append((t, v, charging))
    return out


def soc(x):
    v = [3.00, 3.45, 3.68, 3.74, 3.77, 3.79, 3.82, 3.87, 3.93, 4.00, 4.10, 4.20]
    p = [0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
    if x <= v[0]:
        return 0.0
    if x >= v[-1]:
        return 100.0
    for i in range(len(v) - 1):
        if x <= v[i + 1]:
            return p[i] + (x - v[i]) / (v[i + 1] - v[i]) * (p[i + 1] - p[i])
    return 100.0


def header(v, charging):
    """The header pill's text, as the display lambda composes it."""
    return 'PANEL CHG' if charging else 'PANEL %.0f%%' % soc(v)


def real_trace(hours=24):
    env = dict(re.findall(r'^([A-Z_]+)=(.*)$', open('/home/astroboy/.hermes/.env').read(), re.M))
    url = env.get('HASS_URL', 'http://192.168.1.103:8123').strip('"').rstrip('/')
    start = dt.datetime.now(dt.timezone.utc) - dt.timedelta(hours=hours)
    req = urllib.request.Request(
        f'{url}/api/history/period/{start:%Y-%m-%dT%H:%M:%S+00:00}'
        f'?filter_entity_id={HIST}&minimal_response',
        headers={'Authorization': 'Bearer ' + env['HASS_TOKEN'].strip('"')})
    raw = json.load(urllib.request.urlopen(req, timeout=25))[0]
    pts = [(dt.datetime.fromisoformat(p.get('last_changed') or p['last_updated']), float(p['state']))
           for p in raw
           if str(p.get('state', '')).replace('.', '', 1).replace('-', '', 1).isdigit()]
    grid, j = [], 0
    t = pts[0][0]
    while t <= pts[-1][0]:
        while j + 1 < len(pts) and pts[j + 1][0] <= t:
            j += 1
        grid.append((t, pts[j][1]))
        t += dt.timedelta(seconds=POLL_S)
    return grid


def transitions(result):
    """State changes, excluding the initial state (which is not a change)."""
    out, last = [], result[0][2]
    for t, v, c in result:
        if c != last:
            out.append((t, v, c))
            last = c
    return out


def longest_low_run_while_charging(result):
    """Longest run of consecutive sub-LOW_V polls still reported as CHG."""
    worst = run = 0
    for _, v, c in result:
        run = run + 1 if (c and v < LOW_V) else 0
        worst = max(worst, run)
    return worst


def main():
    fails = []

    def check(name, ok, detail=''):
        print('  %-58s %s %s' % (name, 'PASS' if ok else 'FAIL', detail))
        if not ok:
            fails.append(name)

    print('=== 1. real 24 h trace ===')
    grid = real_trace()
    res = state_machine(grid)
    tr = transitions(res)
    print('  %d polls at %d s, %.3f..%.3f V' % (len(grid), POLL_S, min(v for _, v in grid),
                                                 max(v for _, v in grid)))
    for t, v, c in tr:
        print('   %s  ->  %-4s  at %.3f V  (header: %s)'
              % (t.astimezone().strftime('%H:%M'), 'CHG' if c else 'BATT', v, header(v, c)))
    check('transitions are bounded (no flapping)', len(tr) <= MAX_TRANSITIONS,
          '(%d transitions, limit %d)' % (len(tr), MAX_TRANSITIONS))
    check('labels the CV plateau as CHG', any(c and v >= ENTER for _, v, c in res))
    chg_t = [t for t, v, c in res if c]
    span = (chg_t[-1] - chg_t[0]).total_seconds() if chg_t else 0
    held = sum(POLL_S for _, v, c in res if c)
    check('CHG does not flicker across its own window',
          bool(span) and held / span >= MIN_CHG_COVERAGE,
          '(%d%% of a %d min window)' % (100 * held / span if span else 0, span / 60))
    check('ends on BATT (the panel is on its cell)',
          res[-1][2] is False, '(%.3f V)' % res[-1][1])
    check('steady low readings are BATT (lingers <= %d poll)' % DEBOUNCE,
          longest_low_run_while_charging(res) <= DEBOUNCE,
          '(longest %d poll)' % longest_low_run_while_charging(res))

    print('=== 2. synthetic CV plateau with real-world noise (+-15 mV) ===')
    rnd = random.Random(7)
    t0 = dt.datetime(2026, 1, 1, 10, 0)
    plateau = [(t0 + dt.timedelta(seconds=POLL_S * i), 4.20 + rnd.uniform(-0.015, 0.015))
               for i in range(240)]
    pres = state_machine(plateau)
    first = next((i for i, (_, _, c) in enumerate(pres) if c), None)
    check('latches early on a noisy plateau', first is not None and first <= 20,
          '(entry at poll %s; mean sits exactly on the %.2f V threshold here)' % (first, ENTER))
    check('holds CHG for >= 90%% of the plateau',
          sum(1 for _, _, c in pres if c) / len(pres) >= 0.9)
    check('the whole plateau stays clear of the release floor', min(v for _, v in plateau) > LEAVE,
          '(min %.3f V vs floor %.2f V)' % (min(v for _, v in plateau), LEAVE))

    print('=== 3. synthetic bulk charge (below the plateau) ===')
    ramp = [(t0 + dt.timedelta(seconds=POLL_S * i), 3.30 + 0.0066 * i) for i in range(120)]
    check('bulk charge reads BATT, not CHG (documented scope)',
          not any(c for _, v, c in state_machine(ramp)),
          '(%.2f->%.2f V)' % (ramp[0][1], ramp[-1][1]))

    print('=== 4. a full cell at rest is not charging ===')
    rest = [(t0 + dt.timedelta(seconds=POLL_S * i), 4.16 - 0.0001 * i) for i in range(60)]
    check('4.16 V resting, no cable -> BATT', not any(c for _, v, c in state_machine(rest)))
    # the same cell with the backlight off (load step of ~+30 mV, measured on the bench)
    lifted = [(t, v + (0.030 if i >= 30 else 0.0)) for i, (t, v) in enumerate(rest)]
    check('a +30 mV sag recovery does not fake a charger',
          not any(c for _, v, c in state_machine(lifted)),
          '(peak %.3f V vs entry %.2f V)' % (max(v for _, v in lifted), ENTER))
    check('a lone 4.19 V poll is not charging (a resting full cell reads that)',
          not any(c for _, v, c in state_machine([(t0, 4.19)])))

    print('=== 5. unplug: latched plateau -> decay ===')
    enc = [(t0 + dt.timedelta(seconds=POLL_S * i), 4.25) for i in range(4)]
    decay = [(enc[-1][0] + dt.timedelta(seconds=POLL_S * (i + 1)), 4.20 - 0.0025 * i)
             for i in range(120)]
    dres = state_machine(enc + decay)
    check('starts latched, then releases exactly once', len(transitions(dres)) == 2,
          '(%d transitions)' % len(transitions(dres)))
    check('stays BATT to the end', dres[-1][2] is False)

    print('=== 6. boot and NAN cases ===')
    check('first poll on the cell (3.95 V) -> BATT', state_machine([(t0, 3.95)])[0][2] is False)
    check('two polls on the plateau -> CHG (the debounce costs 60 s at boot)',
          all(c for _, v, c in state_machine([(t0, 4.25), (t0, 4.25)])[1:]))
    check('NAN is never CHG', state_machine([(t0, float('nan'))])[0][2] is False)

    print()
    if fails:
        print('FAILED: %s' % ', '.join(fails))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
