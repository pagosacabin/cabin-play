# chess-mcp

An MCP server that exposes the Nordtronics chess game as three tools, so an MCP
agent (Hermes) can read a position and play a move instead of speaking the
page's plain-HTTP agent endpoint.

- Transport: **stdio only** (the MCP client spawns the process). No listener, no
  port, no public endpoint.
- Game API: `https://nordtronics.io/chess` (public, no auth). Override with
  `CHESS_API_BASE`.
- No credentials anywhere in this directory.

## Tools

| Tool | Arguments | Returns |
|---|---|---|
| `get_game` | `game_id` | `game_id`, `fen`, `moves` (SAN), `side_to_move`, `status`, `juno_side`, `api_fen` |
| `list_legal_moves` | `game_id` **or** `fen` | `fen`, `moves` (UCI, sorted) |
| `make_move` | `game_id`, `move` (UCI), optional `side` | `game_id`, `move_uci`, `san`, `fen`, `side` |

`make_move` validates the move with python-chess and refuses — submitting
nothing — an illegal move, a move for a side that is not to move, and any move
in a game whose `status` is not `active`. Every failure comes back as
`{"error": "<reason>"}`; no tool raises.

## Start

```bash
# deps (already installed here)
/home/astroboy/.local/share/chess-mcp/venv/bin/pip install -r chess-mcp/requirements.txt

# run over stdio
/home/astroboy/.local/share/chess-mcp/venv/bin/python chess-mcp/server.py
```

MCP client config (Hermes reads `mcp_servers:`):

```yaml
mcp_servers:
  chess:
    command: /home/astroboy/.local/share/chess-mcp/venv/bin/python
    args: ["/home/astroboy/cabin-play/chess-mcp/server.py"]
    env:
      CHESS_API_BASE: https://nordtronics.io/chess
```

## SDK note

`mcp` 2.x renamed the high-level server class: `mcp.server.fastmcp.FastMCP`
became `mcp.server.mcpserver.MCPServer`. `server.py` imports `MCPServer` and
falls back to `FastMCP` so it runs on both 1.x and 2.x. Pinned here:
`mcp==2.3.0`, `chess==1.11.2`.

## The `fen` field (two traps)

1. **It must be the post-move FEN.** The API stores whatever `fen` the client
   posts; the page posts the position *after* the move, and its resume path
   rejects a game whose stored `fen` differs from the position it replays. So
   `make_move` posts the post-move FEN.
2. **It must use the page's dialect.** The page's `toFEN` writes the
   en-passant target *unconditionally* after a double pawn push (`... w KQkq a6
   0 2`), while python-chess omits it unless an ep capture is legal. Posting the
   plain `board.fen()` therefore makes the page's comparison fail after any
   double push, and it silently falls back to a bare position with an empty move
   list. `server.py` emits `board.fen(en_passant="fen")` — the game's own dialect.

Incoming positions are always rebuilt by replaying the SAN list with
python-chess, never by trusting the stored `fen` (`get_game` exposes that as
`api_fen` for transparency only).

## Security notes (what the reviewer should see)

- **No secrets.** Nothing in this tree holds a credential; the game API carries
  none.
- **No auth on the game endpoints — by design, upstream.** The public game API
  at `nordtronics.io/chess` has no authentication, and this task must not add
  one (the page and its agent box cannot send credentials). The boundary is the
  stdio transport: the server opens no port and is reachable only by the MCP
  client that spawns it.
- **TLS.** The base URL must be `https://`; a plain-http base is refused unless
  `CHESS_API_ALLOW_INSECURE=1` is set explicitly for local development.
- **Bounds.** game ids `^[A-Za-z0-9_-]{1,64}$`, UCI moves `^[a-h][1-8][a-h][1-8][qrbn]?$`,
  FENs ≤128 chars and parsed/validated, move lists ≤512 entries, every HTTP read
  capped at 64 KiB with a 10 s timeout.
- **Fail-closed.** Every tool returns `{"error": ...}`; none raises. `make_move`
  re-reads the game after `POST /api/move` and reports an error if the move was
  not actually recorded — an HTTP 200 is treated as a claim, not evidence.

## Proof

`tests/mcp_session_proof.py` spawns the server over real stdio MCP, creates a
throwaway game, exercises both refusals, plays `e4` and `Nf6`, and re-reads the
public game JSON:

```bash
/home/astroboy/.local/share/chess-mcp/venv/bin/python chess-mcp/tests/mcp_session_proof.py
```
