#!/usr/bin/env python3
"""End-to-end proof for the /games game-state API.

Drives the exact request shapes the live page makes (index.html: junoStart,
junoSendAndPoll, the 4 s poll, and the CORS preflight a browser sends for a
JSON POST from another origin), in order, against a base URL:

    python3 proof_games.py https://home.tail2bff3f.ts.net/pocket-conquest

It also replays Juno's answer through the rules engine, so the reply is proved
legal by the engine rather than by trust in the service that produced it.
"""

import json
import sys
import urllib.error
import urllib.request

import pocket_conquest as pc

ORIGIN = "https://nordtronics.io"
BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8767").rstrip("/")


def call(method, path, body=None, origin=None, timeout=35):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if origin:
        req.add_header("Origin", origin)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            return res.status, dict(res.headers), res.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read().decode("utf-8")


def page_payload(position, you):
    """Mirror index.html statePayload(side) exactly."""
    board = {}
    for name in pc.TILE_NAMES:
        tile = position["grid"][name]
        owner = {"S": "nord", "R": "ember"}.get(tile["owner"])
        armies = tile["armies"]
        immobile = armies if tile["moved"] else tile.get("built_armies", 0)
        if tile["terrain"] == "town" and tile["owner"] is None and tile["town"]:
            owner, armies, immobile = "neutral", tile["town"]["militia"], 0
        terrain = {"plain": "plains", "hill": "hills"}.get(tile["terrain"], tile["terrain"])
        board[name] = {
            "terrain": terrain,
            "name": (tile["town"]["name"] if tile["town"] else ""),
            "owner": owner,
            "armies": armies,
            "ready": max(0, armies - immobile),
        }
    return {
        "game": "pocket-conquest",
        "you": you,
        "turn": position["turn"],  # page: Math.ceil(ply / 2)
        "gold": {"nord": position["gold"]["S"], "ember": position["gold"]["R"]},
        "board": board,
        "legal": {"note": "not used by the bridge"},
        "rules": "condensed rules text, ignored by the bridge",
    }


def show(label, status, headers, body, keys=("id", "state", "move", "ok", "error")):
    print("== %s" % label)
    print("   status: %s" % status)
    for key in ("access-control-allow-origin", "access-control-allow-methods",
                "content-type", "content-length"):
        if key in {k.lower() for k in headers}:
            value = {k.lower(): v for k, v in headers.items()}[key]
            print("   %s: %s" % (key, value))
    try:
        doc = json.loads(body)
    except Exception:
        print("   body: %s" % body[:300])
        return None
    summary = {k: doc[k] for k in keys if k in doc}
    if "state" in summary and isinstance(summary["state"], dict):
        st = summary["state"]
        summary["state"] = {k: st.get(k) for k in ("turn", "ply", "winner")}
        summary["state"]["gold"] = st.get("gold")
        summary["state"]["tiles"] = len(st.get("tiles") or {})
    print("   body: %s" % json.dumps(summary)[:400])
    return doc


def main():
    failures = []

    # Unit check for the prompt bound (review pass 1, finding 1): a maximal
    # board must not be able to hand the model thousands of move strings.
    import server

    big = pc.initial_position()
    big["side"] = "R"
    for name in pc.TILE_NAMES:
        tile = big["grid"][name]
        if tile["terrain"] in ("plain", "hill"):
            tile["terrain"] = "plain"
            tile["owner"] = "R"
            tile["armies"] = 12
            tile["moved"] = False
            tile["built_armies"] = 0
    big["gold"]["R"] = 9999
    big_legal = pc.legal_moves(big)
    bounded = server.bounded_legal(big_legal)
    print("== prompt bound: %d legal moves -> %d offered, END kept=%s"
          % (len(big_legal), len(bounded), (not big_legal) or ("END" in big_legal) == ("END" in bounded)))
    if len(big_legal) > server.MAX_LEGAL_FOR_PROMPT and len(bounded) > server.MAX_LEGAL_FOR_PROMPT:
        failures.append("bounded_legal did not bound a %d-move list" % len(big_legal))
    if "END" in big_legal and bounded[-1] != "END":
        failures.append("bounded_legal dropped END")

    nord = pc.initial_position()
    status, headers, body = call("POST", "/games", {
        "game": "pocket-conquest", "mode": "juno", "state": page_payload(nord, "nord"),
    })
    created = show("POST /games (create, as junoStart does)", status, headers, body)
    if status != 200 or not created or not created.get("id"):
        print("FAIL: create did not return a game id")
        return 1
    gid = created["id"]

    # The player's own turn, played by the page locally: one legal nord move.
    nord = pc.apply_move(nord, pc.legal_moves(nord)[0])
    nord = pc.apply_move(nord, "END")
    assert nord["side"] == "R", nord["side"]
    ember_state = page_payload(nord, "ember")

    status, headers, body = call("POST", "/games/%s/move" % gid, {
        "game": "pocket-conquest", "state": ember_state,
    }, origin=ORIGIN, timeout=30)
    moved = show("POST /games/{id}/move (the player's End turn)", status, headers, body)
    if status != 200 or not moved:
        failures.append("move failed")
    juno_move = (moved or {}).get("move")

    if juno_move:
        if not pc.is_legal(nord, juno_move):
            failures.append("Juno's move %r is not legal in the position" % juno_move)
        after = pc.apply_move(pc.apply_move(nord, juno_move), "END")
        print("   engine replay: %r -> turn=%s ply=%s ember_armies=%d" % (
            juno_move, after["side"], after["turn"],
            sum(t["armies"] for t in after["grid"].values() if t["owner"] == "R")))

    status, headers, body = call("GET", "/games/%s" % gid, origin=ORIGIN)
    polled = show("GET /games/{id} (the 4 s poll)", status, headers, body)
    if status != 200 or not polled:
        failures.append("poll failed")
    else:
        state = polled.get("state") or {}
        if state.get("turn") != "nord":
            failures.append("poll did not report nord's turn (got %r)" % state.get("turn"))
        tiles = state.get("tiles") or {}
        if len(tiles) != 30:
            failures.append("polled state has %d tiles" % len(tiles))
        for name in ("B5", "E1"):
            tile = tiles.get(name) or {}
            if not {"owner", "armies", "immobile", "acquiredPly"} <= set(tile):
                failures.append("tile %s is missing adoptState fields" % name)
        print("   adoptState fields present on every tile: %s" % all(
            {"owner", "armies", "immobile", "acquiredPly"} <= set(t) for t in tiles.values()))
        if juno_move:
            replay = page_payload(pc.apply_move(pc.apply_move(nord, juno_move), "END"), "nord")
            same = all(
                (tiles[n].get("owner") == replay["board"][n]["owner"]
                 and tiles[n].get("armies") == replay["board"][n]["armies"])
                for n in pc.TILE_NAMES
            )
            print("   polled board equals the engine replay: %s" % same)
            if not same:
                failures.append("polled state diverges from the engine replay")

    # A replayed (older) state for a live id must be refused, not accepted as a
    # rewind (review pass 3, LOW).
    status, _, body = call("POST", "/games/%s/move" % gid, {
        "game": "pocket-conquest", "state": ember_state,
    }, origin=ORIGIN, timeout=30)
    print("== POST /games/{id}/move replaying ply 2 against a ply-3 game -> %s (expected 400)"
          % status)
    if status != 400:
        failures.append("a rewound state was accepted (status %s)" % status)

    status, headers, body = call("OPTIONS", "/games", origin=ORIGIN)
    print("== OPTIONS /games (browser preflight, Origin %s)" % ORIGIN)
    print("   status: %s" % status)
    allow = {k.lower(): v for k, v in headers.items()}
    print("   access-control-allow-origin: %s" % allow.get("access-control-allow-origin"))
    print("   access-control-allow-methods: %s" % allow.get("access-control-allow-methods"))
    print("   access-control-allow-headers: %s" % allow.get("access-control-allow-headers"))
    if status not in (200, 204):
        failures.append("preflight status %s" % status)
    if allow.get("access-control-allow-origin") != ORIGIN:
        failures.append("preflight ACAO is %r" % allow.get("access-control-allow-origin"))

    status, headers, body = call("GET", "/health")
    print("== GET /health -> %s %s" % (status, body.strip()[:80]))
    if status != 200:
        failures.append("health failed")

    status, headers, body = call("POST", "/pocket-conquest-move", {
        "position": {"side": "R", "gold": {"S": 10, "R": 10}, "turn": 1,
                     "grid": pc.initial_position()["grid"]},
        "moves": [],
    }, timeout=30)
    print("== POST /pocket-conquest-move (0275's endpoint, unchanged) -> %s %s"
          % (status, body.strip()[:120]))
    if status != 200:
        failures.append("legacy move endpoint failed")

    bad = dict(page_payload(pc.initial_position(), "nord"))
    bad["board"] = dict(bad["board"])
    bad["board"].pop("A1")
    status, _, body = call("POST", "/games", {"game": "pocket-conquest", "state": bad})
    print("== POST /games with a 29-tile board -> %s (expected 400)" % status)
    if status != 400:
        failures.append("a malformed board was not rejected")

    # Creation bucket (review pass 2, finding 1): a burst must be throttled, or a
    # tailnet peer can churn the bounded game table and evict live games.
    burst = []
    for _ in range(8):
        burst.append(call("POST", "/games", {
            "game": "pocket-conquest",
            "state": page_payload(pc.initial_position(), "nord"),
        })[0])
    print("== POST /games x8 (create bucket %d/min burst %d) -> %s"
          % (server.CREATE_PER_MIN, server.CREATE_BURST, burst))
    if 429 not in burst:
        failures.append("the create bucket did not throttle a burst of 8")
    if burst.count(200) > server.CREATE_BURST + 1:
        failures.append("more than burst+1 creations were accepted: %s" % burst)

    print("\nFAILURES: %d" % len(failures))
    for item in failures:
        print(" - %s" % item)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
