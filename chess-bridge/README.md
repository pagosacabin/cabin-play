# chess-bridge

A one-move chess endpoint for the "Plug in your own agent" box on
<https://nordtronics.io/chess/>. The page POSTs the position, gets one UCI move
back, and plays it; if anything goes wrong the page falls back to its built-in
computer, so the endpoint is allowed to return HTTP 500 and must never answer a
move that is not legal.

## Contract

```
POST /chess-move
  {
    "fen": "<FEN>",
    "moves": ["e2e4", "e7e5", ...],                     # moves optional
    "difficulty": "easy" | "normal" | "hard",           # optional, default "normal" ("medium" -> "normal")
    "personality": "aggressive" | "teach me" | "default" # optional, default "default"
  }
->  200 {"move": "g8f6", "comment": "..."}              # legal UCI for that FEN, comment may be ""
->  400 {"error": "..."}                               # malformed / illegal input
->  500 {"error": "..."}                               # no move available
GET  /health -> 200 {"ok": true}                         # opaque; details are in the journal
OPTIONS /chess-move -> 204 (CORS preflight)               # 403 for an unknown Origin
->  429 {"error": "too many requests"}                    # rate limit
->  503 {"error": "busy"}                                 # every worker slot taken
```

The browser sends `Content-Type: application/json` cross-origin, so a preflight
is unavoidable. The service answers `OPTIONS` and echoes
`Access-Control-Allow-Origin` only for `https://nordtronics.io` and
`https://www.nordtronics.io`. An unknown origin fails closed: the preflight is
refused with `403` and no `Access-Control-Allow-Origin` header is ever sent for
it (it is not substituted with a trusted origin). CORS is not authentication —
it only decides which browser origins may read the reply.

## Request and response fields (v2)

### Request parameters
* `fen` (string, required): FEN representation of the board position (≤ 128 characters).
* `moves` (list of strings, optional): UCI move history (≤ 512 entries, each ≤ 5 chars).
* `difficulty` (string, optional): One of `"easy"`, `"normal"`, or `"hard"`. `"medium"` is accepted as an alias for `"normal"`. Missing or unrecognized values default to `"normal"`.
  Controls candidate generation in Stockfish MultiPV for the position:
  - **easy**: 3 candidates, depth ceiling 8, time cap 0.25 s
  - **normal**: 4 candidates, depth ceiling 12, time cap 0.45 s
  - **hard**: 5 candidates, depth ceiling 16, time cap 0.80 s
  Depth is a ceiling; the time cap is the hard limit — whichever completes first.
* `personality` (string, optional): Allowlisted strictly to `"aggressive"`, `"teach me"`, or `"default"`. Missing or other values default to `"default"`. Unallowlisted client strings are never pasted into model prompts.

### Response fields
* `move` (string): The chosen legal move in UCI format.
* `comment` (string): A short, plain-language explanation (1–2 sentences) of the move from the coach perspective, including analysis of the player's blunder if one was detected on their last move.
* **Degradation / fallback behaviour**: The comment never holds up the move. If DeepSeek is unavailable, times out, returns an unusable response after retry, or if the handler deadline has no time remaining for a model call, the service immediately falls back to Stockfish's top candidate (`candidate[0]`) and sets `comment: ""`.

## How a move is chosen (v2)

1. Legal moves are computed from the FEN with python-chess.
2. Stockfish MultiPV generates the top N candidate moves for the position with side-to-move evaluations (N, depth ceiling, and time cap determined by `difficulty`).
3. If moves history is present, a shallow pre-move evaluation (depth 8, 0.15 s) checks whether the player's last move dropped evaluation by ≥ 150 centipawns. If so, a BLUNDER note is generated.
4. DeepSeek (`deepseek-flash`, thinking disabled, temperature 0.3, `max_tokens` 220, 2.6 s timeout) is given the candidates, difficulty, personality, and optional blunder note to pick one candidate and explain it in JSON.
5. If the model reply is invalid or missing, it is retried **once** with the error appended if ≥ 0.8 s budget remains.
6. If the model fails or runs out of time, Stockfish's best candidate (`candidate[0]`) is returned immediately with `comment: ""`.
7. If Stockfish cannot answer either, the service returns 500. It never picks a random legal move.

Every request — `GET`, `OPTIONS` and `POST`, including the rejected ones — writes one stderr line:
`chess-move method=… path=… status=… source=model|stockfish|none elapsed_ms=…`.
For POST 200 responses, the journal line also includes: `difficulty=… candidates=… depth=… comment=model|empty`.
A 500 carries `detail=` with the underlying reason in the journal only; the client gets the stable body `{"error": "internal error"}`. No key and no prompt text are ever logged.

## Install (once)

```bash
# python-chess in a dedicated venv
python3 -m venv ~/.local/share/chess-bridge/venv
~/.local/share/chess-bridge/venv/bin/pip install -r requirements.txt

# Stockfish: official static build, SHA-256 pinned, no root needed
./install-stockfish.sh            # installs to ~/.local/bin/stockfish
```

`install-stockfish.sh` pins the release tarball's SHA-256
(`09e95322…e31987d`) and refuses to install anything whose `id name` is not
`Stockfish 17.1`. The engine is re-identified at every service start — a missing engine **or one
whose `id name` is not `Stockfish 17.1` is fatal**, because the pin is a claim
this service makes — and again on every fallback move, so a binary swapped after
startup is refused instead of trusted. The identity is printed in the startup
line of the journal.

The DeepSeek key is **not** in this tree. `run.sh` takes it from
`DEEPSEEK_API_KEY`, or reads that one variable out of `~/.hermes/.env` when it is
not already exported.

## Start / stop

Deployed copy: `~/.local/share/chess-bridge/app` (this directory, copied from the
repo). Source of truth: `cabin-play:chess-bridge/`, branch
`hermes/0268-chess-bridge`.

```bash
# start (foreground)
~/.local/share/chess-bridge/app/run.sh

# start (background, survives the shell)
nohup ~/.local/share/chess-bridge/app/run.sh >> ~/.local/share/chess-bridge/app.log 2>&1 &

# stop
pkill -f 'chess-bridge/app/server.py'

# or as a user unit (not installed by default)
cp chess-bridge.service ~/.config/systemd/user/chess-bridge.service
systemctl --user daemon-reload && systemctl --user enable --now chess-bridge.service
```

## Reboot survival

**It does not come back on its own.** The service runs as a foreground process
started by hand (or via the user unit if that has been installed and enabled by
Stephen). Nothing in this task installed or enabled the unit, and the user unit
itself only starts when the user session starts — the host sits at the greeter
after a reboot until someone logs in. After a reboot, start it with
`~/.local/share/chess-bridge/app/run.sh` (or the unit, if installed).

## Security posture

- **No application authentication, deliberately.** The page cannot send
  credentials (its `fetch` carries only `Content-Type`), and the task forbids
  OAuth on the Serve path. The boundary is the tailnet: Serve publishes the
  path tailnet-only (no funnel), and the process binds `127.0.0.1`. Anyone on
  the tailnet can spend a DeepSeek call; nobody outside it can reach the port.
  This is the one review finding that is a spec constraint rather than a defect —
  the cost side of it is bounded below by the token bucket and the worker pool.
- Input is bounds-checked (8 KiB body, FEN ≤128 chars, ≤512 moves of ≤5 chars
  and each must match the UCI grammar, JSON object shape) and every move is
  validated legal before it is returned.
- **Engine work is admitted, not piled up**: at most two searches are live at
  once and they are serialised behind one lock; a search that outlives the
  handler deadline is abandoned (the handler answers) and its slot is only
  returned when the thread really ends. Past the cap the bridge answers `500`
  with a journal reason, so a stalled engine cannot accumulate processes.
- **Spend is bounded**: a global token bucket (`CHESS_BRIDGE_RATE_PER_MIN`, 60/min,
  burst 10) answers `429` beyond the budget, and at most
  `CHESS_BRIDGE_MAX_CONCURRENCY` (4) requests are worked at once — the next one
  gets `503` + `Retry-After: 1` after 0.25 s. One request draws at most two model
  attempts plus one engine search, all against a single 5.5 s deadline, so a
  caller cannot queue up work past the page's 6 s budget.
- **The upstream URL is allowlisted and redirects are refused**: `CHESS_BRIDGE_URL`
  must be `https`, or `http` to a loopback host, so the bearer key cannot be sent
  in cleartext off-box by an environment override. The service refuses to start
  otherwise, and the HTTP call uses an opener with the redirect handler disabled —
  a 30x from the provider cannot carry the `Authorization` header to a second
  host (it is treated as a failed model call and logged as `detail=model HTTP 3xx`).
- The key is read from the environment only; it is never logged, never
  serialised into a response, and never written to this tree.
- Request headers are bounded **while they are read** (8 KiB total → `431`), not
  after buffering, and the socket read timeout is 5 s, so a slow-drip client
  cannot hold a worker or grow the process with an unbounded header block.
- Unhandled methods (anything but `GET`/`OPTIONS`/`POST`) answer `501` and still
  write their journal line. The `431`/`501` paths are logged like every other
  request.
- TLS terminates at Tailscale Serve; the service speaks loopback HTTP only.
- The packaged unit runs with `NoNewPrivileges`, an empty capability set,
  `ProtectSystem=strict`, `ProtectHome=read-only`, `PrivateTmp`,
  `PrivateDevices`, a `@system-service` syscall filter, `MemoryMax=256M` and
  `TasksMax=32`.

## Exposure

Public path (tailnet only, no funnel, no OAuth) proxying `http://127.0.0.1:8765`:

```
https://home.tail2bff3f.ts.net/chese-move        # the mount as configured on this host
https://home.tail2bff3f.ts.net/chese-move/chess-move
```

The mount was created as root by Stephen with a typo — `chese-move`, not
`chess-move` — so that path is the live one. To make the spelling match the
name, run once as root (it adds the correct mount; the service is unchanged):

```bash
sudo tailscale serve --bg --https=443 --set-path /chess-move http://127.0.0.1:8765
sudo tailscale serve --https=443 --set-path /chese-move off   # optional tidy-up
```

`tailscale serve status` is readable without root and shows the live mounts.
