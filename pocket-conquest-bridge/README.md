# pocket-conquest-bridge

An agent endpoint for the Pocket Conquest page. The page POSTs the current
position and the legal moves it knows about; the endpoint answers with one legal
move chosen by DeepSeek (the same key and model Hermes uses), or with an error so
the page falls back to its built-in computer.

```
POST /pocket-conquest-move
     {"position": {...}, "moves": ["M B5 B4 2", ...]}
  -> 200 {"move": "M C5 C4 5"}
  -> 400 {"error": "..."}      malformed or unknown position (fail-closed)
  -> 429 {"error": "..."}      rate limit
  -> 500 {"error": "..."}      the model produced no legal move (page falls back)

GET /health -> 200 {"ok": true}
```

Nothing is installed beyond the Python 3 standard library (3.11+). The DeepSeek
key is read from `DEEPSEEK_API_KEY` (or from `~/.hermes/.env` via `run.sh`) and
never written to a file, logged, or echoed.

## Files

| File | Purpose |
|---|---|
| `pocket_conquest.py` | Rules engine: legal-move generation, move validation, combat, income, turn handoff, `--selftest` |
| `agent_core.py` | Prompt builder (rules + board + gold + armies + legal list + tactics hints), reply parser, retrying `pick_legal` |
| `server.py` | The HTTP service: validation, one deadline for the whole request, rate limit, worker pool, journal |
| `proof.py` | End-to-end check against a running instance; every returned move is re-validated by the engine |
| `run.sh` | Foreground start |
| `pocket-conquest-bridge.service` | systemd user unit for the deployed copy |

## Position JSON schema

```json
{
  "side": "S",
  "gold": {"S": 10, "R": 10},
  "turn": 1,
  "grid": {
    "B5": {"terrain": "capital", "owner": "S", "town": null, "capital": "S",
           "armies": 3, "moved": false, "built_armies": 0},
    "...": {}
  },
  "captured_this_turn": []
}
```

- `side`: `"S"` (Nordhold) or `"R"` (Ember Court) — the side to move.
- `gold`: `{"S": int, "R": int}`, or a single int for the side to move.
- `turn`: positive integer; increments when `"R"` ends a turn.
- `grid`: all 30 tiles keyed `"A1"`..`"F5"`.
  - `terrain`: `"plain" | "hill" | "town" | "capital"`.
  - `owner`: `"S" | "R" | null`.
  - `town`: `null`, or `{"name": str, "militia": int}`.
  - `capital`: `null`, or `"S" | "R"`.
  - `armies`: non-negative integer of occupying armies (`0` if empty).
  - `moved`: true if the stack here already moved/attacked this turn.
  - `built_armies`: how many armies here were raised this turn — only those are
    immobile. `built` is accepted as a legacy flag and means `built_armies = armies`.
- `captured_this_turn`: tiles captured by the side that just moved (no building
  there until next turn).
- `winner` (optional): `"S"` or `"R"` once a capital has been taken.

`normalise_position(raw)` also accepts `"board"` as an alias for `"grid"`, a 5x6
row-major list of cells, and cells written in the compact diagram form: `.`
(plain), `H` (hill), `T2` (neutral town, militia 2), `S3*` (a stack of 3 on its
capital), `R2T` (a stack on its own town), trailing `~` for moved/just-built.
Anything else raises `ValueError`, which the service turns into a 400.

## Move string grammar

Moves are single-space-delimited ASCII strings:

| Move | Meaning |
|---|---|
| `B B5 2` | build 2 armies at B5 (5 gold each; capital or an owned town only) |
| `M B5 B4 3` | move 3 armies orthogonally from B5 to B4 |
| `M B5 B4 3 + C3 2` | the same attack with C3 contributing 2 armies as pincer support |
| `END` | end the turn: collect income, clear the moved/built flags, hand over |

A side is never forced to move, so `END` is always legal and is how a turn
finishes. Combat is resolved in the engine only to validate a move; the page
keeps the authoritative board.

A malformed or illegal move string gives `is_legal(position, move) == False`, and
`apply_move` raises `ValueError`.

## What the model is told, every move

`agent_core.build_prompt()` sends the condensed but complete rules, a strategy
line, the board, whose turn it is, both sides' gold and army totals, a `TACTICS:`
line naming any capital-taking or clearly favourable attack, and the numbered
list of legal moves. It asks for exactly one entry from that list and nothing
else. A reply is accepted only if it is in the list — a bare list number, a
quoted move, or a move inside a sentence all parse; anything else is retried once
with the error appended, and then the endpoint answers 500 so the page uses its
own computer. It never answers a random legal move.

Measured on 2026-11-27 with deepseek-flash (`thinking: disabled`): 384-756 prompt
tokens (0-512 of them cache hits) and 1-9 completion tokens per move, 0.6-0.8 s
round trip over 12 logged calls.

## Run it

```bash
~/.local/share/pocket-conquest-bridge/app/run.sh          # foreground
systemctl --user start|stop|restart pocket-conquest-bridge.service
journalctl --user -u pocket-conquest-bridge.service -f
```

The unit is a **user** unit: this host boots to a greeter, so the service starts
with the user session, not with the kernel — it does not come back on its own at
boot.

## Tailscale Serve

The page is HTTPS, so the endpoint must be too. Serve config on this host belongs
to root; the one command that publishes the service on the tailnet is

```bash
sudo tailscale serve --bg --https=443 --set-path /pocket-conquest http://127.0.0.1:8767
```

giving `https://home.tail2bff3f.ts.net/pocket-conquest`. A worker run cannot write
serve config (`Access denied: serve config denied`), so that is Stephen's command.

**Paste into the page's agent box:** `https://home.tail2bff3f.ts.net/pocket-conquest`

`tailscale serve --set-path` keeps the mount prefix in the forwarded path (proven
on the chess bridge, where the upstream sees `/chese-move/chess-move`), so the
service accepts the POST at the mount root (`/pocket-conquest`), with a trailing
slash, at `/pocket-conquest/pocket-conquest-move`, and at the bare
`/pocket-conquest-move` for a direct localhost call. `GET` health answers at
`/health` and `/pocket-conquest/health`.

## Self-test

```bash
python3 pocket_conquest.py --selftest   # rules engine (runs agent_core's too)
python3 agent_core.py --selftest        # prompt builder and reply parser
python3 proof.py http://127.0.0.1:8767  # end-to-end against a running instance
```
