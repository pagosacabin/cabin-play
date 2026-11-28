# pocket-conquest-mcp

An MCP server that lets a cold agent play **Pocket Conquest** through three tools instead of a
browser. stdio transport only — no port, no public endpoint, no credentials.

```
get_game(game_id="")      read the position (or create a new game when game_id is empty)
list_legal_moves(game_id) the complete legal move list for the side to move
make_move(game_id, move)  validate one move with the rules engine and submit it
```

The legality logic is **not** reimplemented here: the server imports
`../pocket-conquest-bridge/pocket_conquest.py` (task 0275's engine) and uses its
`legal_moves` / `is_legal` / `apply_move` / `winner`. The server itself makes no model calls and
holds no keys.

## Run

```bash
# terminal API stand-in (test harness; the live VPS API is Juno's to stand up)
python3 local_api.py --port 8791   # store: ~/.cache/pocket-conquest-standin/games (mode 0700)

# the MCP server, spoken to over stdio by any MCP client
POCKET_CONQUEST_API_BASE=http://127.0.0.1:8791 python3 server.py
```

`POCKET_CONQUEST_API_BASE` defaults to `http://127.0.0.1:8791`. Plain `http` is accepted only for
loopback/private addresses; anything else must be `https://`.

Dependencies: Python 3.11+, the `mcp` package (`requirements.txt`), and the sibling
`pocket-conquest-bridge/` directory (imported in place — the two live in the same repo).

## The game

Six columns (A–F) by five rows (1–5), thirty tiles, orthogonal movement only.
**Nordhold (S)** — the side the calling agent plays — versus **Ember Court (R)**. Each side starts
with 3 armies at its capital and 10 gold.

```
     A     B     C     D     E     F
 1   .     H     .     T1    R3*   H
 2   .     .     H     .     .     .
 3   .     T2    .     .     .     .
 4   .     .     .     H     .     .
 5   H     S3*   .     .     T1    .
```

`*` capital · `H` hills (defender +1) · `T<n>` neutral town with `n` militia.
Capitals: B5 = Nordhold (S), E1 = Emberhold (R). Towns: D1 Northford (1), B3 Crossroads (2),
E5 Southmere (1). Hills: B1, C2, D4, F1, A5.

**A turn** is: (1) Build — 5 gold per army, only at your capital or a town you own; armies built
this turn cannot move; a town captured this turn cannot be built at until next turn.
(2) Move / Attack — one tile per stack; a stack moves once; arrivals cannot move again or join
another attack that turn; stacks may split per army; moving into an empty tile claims it, into a
neutral or enemy town takes it if its militia is destroyed, into an enemy stack is an attack.
(3) Income at end of turn: capital 5, each owned town 4, each owned plain tile 2.

**Combat**: attacker = armies + 1d6; defender = armies + 1d6, +1 on hills. Higher total wins; ties
go to the defender. The loser loses 1 army, or 2 if the margin is 3+. A tile falls only when its
defenders reach zero. **Pincer**: an adjacent friendly stack can join one attack — single roll,
armies summed, the supporting stack stays put.
**Win**: occupy the enemy capital.

## Move grammar

| Form | Meaning |
|---|---|
| `B <tile> <n>` | build `n` armies at a capital or owned town you control |
| `M <src> <dst> <n>` | move or attack with `n` armies one tile orthogonally |
| `M <src> <dst> <n> + <src2> <n2>` | pincer: `src2` supports the attack with `n2` armies |
| `END` | collect income and hand the turn over |

Every field is named, so the grammar is unambiguous; `list_legal_moves` is authoritative and the
server refuses anything not in it.

## Game API contract

The server speaks three endpoints (the stand-in in `local_api.py` implements exactly this):

| Endpoint | Request | Response |
|---|---|---|
| `POST /pocket-conquest/api/new` | `{}` | `201 {game_id, status, side_to_move, winner, position}` |
| `POST /pocket-conquest/api/move` | `{game_id, move, from_digest}` | `200 {…, accepted: true}`, or `409 {error}` |
| `GET /pocket-conquest/games/<id>.json` | — | `{game_id, status, side_to_move, winner, position, moves}` |

`from_digest` is the digest of the state the move was computed from — the API rejects a move built
on an older state (`409`) instead of overwriting it. The API re-validates the move itself with the
rules engine and derives the new position from its own copy; **no client-supplied position is
accepted**, so a client bug cannot move the game into an illegal state and a compromised client
cannot inject one. A move is only reported as played after a re-fetch shows it in the stored game.

## Proof

```bash
python3 tests/mcp_session_proof.py --api-port 8791
```

Spawns the stand-in, connects a real MCP client over stdio, creates a game, reads it, lists the
legal moves, plays one, verifies it landed in the game JSON, and exercises the refusals (illegal
move, wrong side, finished game, malformed input). Exit code 0 with `N/N checks passed`.
