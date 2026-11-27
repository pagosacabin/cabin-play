#!/usr/bin/env python3
"""End-to-end proof for the Pocket Conquest agent endpoint.

Sends real positions to a running bridge, checks every returned move against the
rules engine, and prints one line per call with the latency. Exit code 1 if any
call failed or returned an illegal move.

    python3 proof.py http://127.0.0.1:8767 > curl-local-proof.txt
"""

import json
import sys
import time
import urllib.request

import pocket_conquest as pc

URL = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8767") + "/pocket-conquest-move"


def midgame_position():
    """A position where building, moving and attacking are all legal."""
    base = pc.initial_position()
    grid = base["grid"]
    # Stephen: capital 4, a stack at C5 that can attack C4, 3 gold... and 15 gold
    # so a build at the capital is also on the table.
    grid["B5"]["armies"] = 4
    grid["C5"] = {"terrain": "plain", "owner": "S", "armies": 5, "moved": False}
    grid["E5"]["owner"] = "S"          # Southmere, taken earlier
    grid["E5"]["town"]["militia"] = 0
    # The rival: capital 3, a stack at C4 (adjacent to C5), Northford 2.
    grid["C4"] = {"terrain": "plain", "owner": "R", "armies": 3, "moved": False}
    grid["D1"]["owner"] = "R"
    grid["D1"]["town"]["militia"] = 0
    grid["D1"]["armies"] = 2
    base["gold"] = {"S": 15, "R": 12}
    return pc.normalise_position(base)


def siege_position():
    """Stephen is next to the undefended Emberhold capital: the win is available."""
    base = pc.initial_position()
    grid = base["grid"]
    grid["B5"]["armies"] = 3
    grid["E2"] = {"terrain": "plain", "owner": "S", "armies": 6, "moved": False}
    grid["E1"]["armies"] = 1
    base["gold"] = {"S": 5, "R": 8}
    return pc.normalise_position(base)


def describe_kind(position, move):
    """Name what the chosen move actually does: build, attack, move, or end turn."""
    parsed = pc._parse_move(move)
    if not parsed:
        return "unparsed"
    if parsed[0] == "B":
        return "build"
    if parsed[0] == "END":
        return "end turn"
    _, src, dst, count, sup, sup_count = parsed
    if pc._is_attack_target(position["grid"][dst], position["side"]):
        return "attack %s->%s with %d" % (src, dst, count + sup_count)
    return "move %s->%s with %d" % (src, dst, count)


def kinds(legal, position):
    """Summarise the option list as build/move/attack/end counts."""
    counts = {"build": 0, "move": 0, "attack": 0, "end turn": 0}
    for move in legal:
        kind = describe_kind(position, move)
        key = "end turn" if kind == "end turn" else kind.split()[0]
        counts[key] += 1
    return ", ".join("%d %s" % (v, k) for k, v in counts.items() if v)


def post(position, page_moves):
    legal = pc.legal_moves(position)
    listed = [m for m in page_moves if m in set(legal)] or legal
    body = json.dumps({"position": position, "moves": listed}).encode()
    request = urllib.request.Request(URL, data=body, method="POST")
    request.add_header("Content-Type", "application/json")
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=12) as response:
            payload = json.loads(response.read().decode())
            status = response.status
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode() or "{}"), time.monotonic() - started, legal
    return status, payload, time.monotonic() - started, legal


def main():
    cases = [
        ("initial position (S to move: build or move from B5)",
         pc.initial_position()),
        ("after a build, an advance and END (R to move)",
         pc.apply_move(pc.apply_move(pc.apply_move(pc.initial_position(), "B B5 2"),
                                    "M B5 B4 3"), "END")),
        ("midgame: build at B5, move C5, or attack C4",
         midgame_position()),
        ("siege: Stephen can take Emberhold this turn",
         siege_position()),
    ]
    failures = 0
    for label, position in cases:
        status, payload, elapsed, legal = post(position, pc.legal_moves(position))
        move = payload.get("move")
        if status == 200 and move in set(legal) and pc.is_legal(position, move):
            kind = describe_kind(position, move)
            print("%-58s -> %-16s %5.2fs  legal (%s; chose from %d options: %s)"
                  % (label, move, elapsed, kind, len(legal), kinds(legal, position)))
            after = pc.apply_move(position, move)
            print("%-58s    side now %s, gold S:%d R:%d, turn %d"
                  % ("", after["side"], after["gold"]["S"], after["gold"]["R"], after["turn"]))
        else:
            failures += 1
            print("%-58s -> HTTP %s %s %5.2fs  FAIL" % (label, status, payload, elapsed))
    # Fail-closed checks: a malformed body and an unknown path must not 200.
    for label, url, body in (
        ("garbage body", URL, b"{not json"),
        ("missing position", URL, b'{"moves": []}'),
    ):
        request = urllib.request.Request(url, data=body, method="POST")
        request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                status = response.status
        except urllib.error.HTTPError as exc:
            status = exc.code
        ok = status == 400
        failures += 0 if ok else 1
        print("%-58s -> HTTP %s  %s" % (label, status, "fail-closed" if ok else "FAIL"))
    print("\n%d failing case(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
