# chess-bridge

A one-move chess endpoint for the "Plug in your own agent" box on
<https://nordtronics.io/chess/>. The page POSTs the position, gets one UCI move
back, and plays it; if anything goes wrong the page falls back to its built-in
computer, so the endpoint is allowed to return HTTP 500 and must never answer a
move that is not legal.

## Contract

```
POST /chess-move
  {"fen": "<FEN>", "moves": ["e2e4", "e7e5", ...]}     # moves optional
->  200 {"move": "g8f6"}                               # legal UCI for that FEN
->  400 {"error": "..."}                               # malformed / illegal input
->  500 {"error": "..."}                               # no move available
GET  /health -> 200 {"ok": true, "model": "...", "stockfish": true}
OPTIONS /chess-move -> 204 (CORS preflight)
```

The browser sends `Content-Type: application/json` cross-origin, so a preflight
is unavoidable. The service answers `OPTIONS` and echoes
`Access-Control-Allow-Origin` for `https://nordtronics.io` and
`https://www.nordtronics.io` (anything else gets `https://nordtronics.io`).

## How a move is chosen

1. Legal moves are computed from the FEN with python-chess.
2. DeepSeek (`deepseek-flash`, thinking disabled, temperature 0, `max_tokens`
   16, 4 s timeout) is asked for one move from that list.
3. An answer that is not in the list, or that is empty, is retried **once** with
   the error text and the same position and list.
4. If that fails too, Stockfish 17.1 plays `go movetime 400` for the position and
   its `bestmove` is used (validated legal before it is returned).
5. If Stockfish cannot answer either, the service returns 500. It never picks a
   random legal move.

Every request writes one stderr line: `chess-move status=… source=model|stockfish
elapsed_ms=…`. No key and no prompt text are logged.

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
`Stockfish 17.1`. The same version is re-checked at every service start and
reported as `GET /health` → `{"stockfish": "Stockfish 17.1"}`, so a swapped
binary is visible rather than silent.

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
  This is the one review finding that is a spec constraint rather than a defect.
- Input is bounds-checked (8 KiB body, FEN ≤128 chars, ≤512 moves of ≤5 chars,
  JSON object shape) and every move is validated legal before it is returned.
- The key is read from the environment only; it is never logged, never
  serialised into a response, and never written to this tree.
- TLS terminates at Tailscale Serve; the service speaks loopback HTTP only.
- The packaged unit runs with `NoNewPrivileges`, an empty capability set,
  `ProtectSystem=strict`, `ProtectHome=read-only`, `PrivateTmp`,
  `PrivateDevices`, a `@system-service` syscall filter, `MemoryMax=256M` and
  `TasksMax=32`.

## Exposure

Intended public path: `https://home.tail2bff3f.ts.net/chess-move` (tailnet only,
no funnel, no OAuth) proxying `http://127.0.0.1:8765`.

That mapping is **not yet configured**: the host's tailscale has no operator
set, so `tailscale serve` requires root and the worker cannot run it. One
command as root enables it:

```bash
sudo tailscale serve --bg --https=443 --set-path /chess-move http://127.0.0.1:8765
```

Until then the service answers on `http://127.0.0.1:8765` only.
