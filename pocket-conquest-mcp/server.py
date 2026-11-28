"""MCP server for Pocket Conquest – exposes three tools over stdio.

This server bridges the pocket-conquest rules engine (pocket_conquest.py) to any
MCP-compatible client.  It manages game state through a REST back-end whose base
URL is configured via the POCKET_CONQUEST_API_BASE environment variable.

No credentials, API keys, or network listeners are required; the server speaks
only over stdio as the MCP transport.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

# ---------------------------------------------------------------------------
# Engine import – resolve the sibling bridge directory at import time so the
# package does not need to be installed.
# ---------------------------------------------------------------------------
_BRIDGE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "pocket-conquest-bridge",
)
if _BRIDGE_DIR not in sys.path:
    sys.path.insert(0, _BRIDGE_DIR)

import pocket_conquest as pc  # noqa: E402

# ---------------------------------------------------------------------------
# MCP import (mcp 2.x; FastMCP was renamed to MCPServer in 2.0)
# ---------------------------------------------------------------------------
from mcp.server.mcpserver import MCPServer  # type: ignore[import-untyped]

# ---------------------------------------------------------------------------
# Logging – stderr only; stdout is the MCP wire.
# ---------------------------------------------------------------------------
logging.basicConfig(stream=sys.stderr, level=logging.INFO,
                    format="%(levelname)s [pocket-conquest] %(message)s")
log = logging.getLogger("pocket-conquest")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
_LOOPBACK_RE = re.compile(
    r"^https?://("
    r"localhost"
    r"|127(?:\.\d{1,3}){3}"
    r"|::1"
    r"|10(?:\.\d{1,3}){3}"          # RFC-1918 10/8
    r"|172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2}"  # RFC-1918 172.16-31/12
    r"|192\.168(?:\.\d{1,3}){2}"    # RFC-1918 192.168/16
    r")"
)

def _get_api_base() -> str | None:
    """Return the validated API base URL, or None with an error already logged."""
    raw = os.environ.get("POCKET_CONQUEST_API_BASE", "http://127.0.0.1:8791").rstrip("/")
    if raw.startswith("https://"):
        return raw
    if _LOOPBACK_RE.match(raw):
        return raw
    return None


_GAME_ID_RE = re.compile(r"\A[A-Za-z0-9_\-]{1,64}\Z")
_MAX_BODY = 65536   # 64 KiB
_TIMEOUT = 10       # seconds

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def _http_get(url: str) -> dict[str, Any]:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as resp:
            body = resp.read(_MAX_BODY)
        return json.loads(body)
    except urllib.error.HTTPError as exc:
        body = exc.read(_MAX_BODY).decode(errors="replace")
        return {"error": f"HTTP {exc.code} from {url}: {body[:300]}"}
    except urllib.error.URLError as exc:
        return {"error": f"Network error fetching {url}: {exc.reason}"}
    except Exception as exc:  # noqa: BLE001
        return {"error": f"Unexpected error fetching {url}: {exc}"}


def _http_post(url: str, payload: dict[str, Any]) -> dict[str, Any]:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as resp:
            body = resp.read(_MAX_BODY)
        return json.loads(body)
    except urllib.error.HTTPError as exc:
        body = exc.read(_MAX_BODY).decode(errors="replace")
        return {"error": f"HTTP {exc.code} from {url}: {body[:300]}"}
    except urllib.error.URLError as exc:
        return {"error": f"Network error posting to {url}: {exc.reason}"}
    except Exception as exc:  # noqa: BLE001
        return {"error": f"Unexpected error posting to {url}: {exc}"}


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _validate_game_id(game_id: str) -> str | None:
    """Return None if valid, or an error string."""
    if not _GAME_ID_RE.match(game_id):
        return (
            "game_id must match [A-Za-z0-9_-]{1,64} "
            f"(got {game_id!r})"
        )
    return None


def _position_digest(pos: dict[str, Any]) -> str:
    """Canonical digest of a whole position (owner, armies, gold, flags).

    A summary of turn/side/army counts is not enough: a move that claims a tile
    can leave every count unchanged while the owner changes, so the whole
    canonical JSON is hashed.
    """
    blob = json.dumps(pos, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def _build_game_response(raw: dict[str, Any], api_base: str) -> dict[str, Any]:
    """Normalise a raw server response into the standard get_game shape."""
    if "error" in raw:
        return raw

    # The server may return the position nested or flat.
    pos_raw: dict[str, Any] | None = raw.get("position") or raw
    try:
        pos = pc.normalise_position(pos_raw)
    except Exception as exc:  # noqa: BLE001
        log.warning("Could not normalise position: %s", exc)
        pos = None

    game_id = raw.get("game_id", raw.get("id", ""))
    status = raw.get("status", "unknown")
    win = None
    side_to_move = raw.get("side", raw.get("side_to_move", "?"))
    turn = raw.get("turn", 0)
    gold = raw.get("gold", {})
    armies: dict[str, int] = {}
    board = ""
    legal: list[str] = []

    if pos is not None:
        win = pc.winner(pos)
        side_to_move = pos["side"]
        turn = pos["turn"]
        gold = pos["gold"]
        for tile, data in pos["grid"].items():
            if data.get("armies", 0):
                armies[tile] = data["armies"]
        board = pc.describe(pos)
        legal = pc.legal_moves(pos)
        if status == "unknown":
            status = "finished" if win else "in_progress"

    return {
        "game_id": game_id,
        "status": status,
        "side_to_move": side_to_move,
        "winner": win,
        "turn": turn,
        "gold": gold,
        "armies": armies,
        "board": board,
        "legal_moves": legal,
        "rules_summary": (
            "Pocket Conquest – 6×5 grid (cols A-F, rows 1-5). "
            "You play side S (Nordhold); opponent is R. "
            "Moves: 'B <tile> <n>' (build n armies, costs 5g each), "
            "'M <src> <dst> <n>' (move n armies, orthogonal only), "
            "'M <src> <dst> <n> + <src2> <n2>' (pincer attack with support), "
            "'END' (end your turn). "
            "Newly built armies cannot move the same turn. "
            "Capture the opponent's capital to win. "
            "Call list_legal_moves for the authoritative move list."
        ),
    }


# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------
mcp = MCPServer(name="pocket-conquest")


@mcp.tool()
def get_game(game_id: str = "") -> dict:  # type: ignore[return]
    """Fetch or create a Pocket Conquest game and return its full state.

    Game: Pocket Conquest – a two-player strategy game on a 6×5 grid (cols A-F,
    rows 1-5). You play side S (Nordhold); the opponent is R. Movement is
    orthogonal only. Win by occupying the enemy capital.

    Move grammar:
      'B <tile> <n>'                      – build n armies (costs 5g each)
      'M <src> <dst> <n>'                 – move n armies one step
      'M <src> <dst> <n> + <src2> <n2>'  – pincer: main attack + support
      'END'                               – end your turn

    Use list_legal_moves for the authoritative list of legal moves.

    Args:
        game_id: Existing game ID (regex [A-Za-z0-9_-]{1,64}). If empty a new
                 game is created via POST to the API and its ID is returned.

    Returns:
        dict with keys: game_id, status, side_to_move, winner, turn, gold,
        armies (tile→count), board (text diagram), legal_moves, rules_summary.
        On failure: {"error": "<message>"}.
    """
    api_base = _get_api_base()
    if api_base is None:
        return {"error": (
            "POCKET_CONQUEST_API_BASE must be https:// or a loopback/private address. "
            "Plain http is only allowed for local test servers."
        )}

    if not game_id:
        url = f"{api_base}/pocket-conquest/api/new"
        raw = _http_post(url, {})
        return _build_game_response(raw, api_base)

    err = _validate_game_id(game_id)
    if err:
        return {"error": err}

    url = f"{api_base}/pocket-conquest/games/{urllib.parse.quote(game_id, safe='')}.json"
    raw = _http_get(url)
    return _build_game_response(raw, api_base)


@mcp.tool()
def list_legal_moves(game_id: str) -> dict:  # type: ignore[return]
    """Return the authoritative list of legal moves for a Pocket Conquest game.

    Game: Pocket Conquest – 6×5 grid (cols A-F, rows 1-5). You play S
    (Nordhold); opponent is R. Movement is orthogonal only.

    Move grammar:
      'B <tile> <n>'                      – build n armies (costs 5g each)
      'M <src> <dst> <n>'                 – move n armies one step
      'M <src> <dst> <n> + <src2> <n2>'  – pincer attack with support flank
      'END'                               – end your turn (always legal)

    This tool re-computes legality from the engine and is authoritative. Always
    call this before choosing a move; never guess or invent move strings.

    Args:
        game_id: Game ID (regex [A-Za-z0-9_-]{1,64}).

    Returns:
        {"game_id", "side_to_move", "moves": [...], "count": N}.
        On failure: {"error": "<message>"}.
    """
    api_base = _get_api_base()
    if api_base is None:
        return {"error": (
            "POCKET_CONQUEST_API_BASE must be https:// or a loopback/private address."
        )}

    err = _validate_game_id(game_id)
    if err:
        return {"error": err}

    url = f"{api_base}/pocket-conquest/games/{urllib.parse.quote(game_id, safe='')}.json"
    raw = _http_get(url)
    if "error" in raw:
        return raw

    pos_raw = raw.get("position") or raw
    try:
        pos = pc.normalise_position(pos_raw)
    except Exception as exc:  # noqa: BLE001
        return {"error": f"Could not parse game position: {exc}"}

    moves = pc.legal_moves(pos)
    return {
        "game_id": game_id,
        "side_to_move": pos["side"],
        "moves": moves,
        "count": len(moves),
    }


@mcp.tool()
def make_move(game_id: str, move: str) -> dict:  # type: ignore[return]
    """Submit a move in a Pocket Conquest game after full local validation.

    Game: Pocket Conquest – 6×5 grid (cols A-F, rows 1-5). You play S
    (Nordhold); opponent is R. Movement is orthogonal only.

    Move grammar:
      'B <tile> <n>'                      – build n armies (costs 5g each)
      'M <src> <dst> <n>'                 – move n armies one step
      'M <src> <dst> <n> + <src2> <n2>'  – pincer attack with support flank
      'END'                               – end your turn (always legal)

    The move is validated locally (grammar + is_legal) before any network call.
    After POST the game is re-fetched to confirm the server accepted the move.
    Always call list_legal_moves first; never guess or invent move strings.

    Args:
        game_id: Game ID (regex [A-Za-z0-9_-]{1,64}).
        move:    Move string in the grammar above.

    Returns:
        {"game_id", "move", "status", "side_to_move", "winner", "board",
         "armies"}. On failure or validation error: {"error": "<message>"}.
    """
    api_base = _get_api_base()
    if api_base is None:
        return {"error": (
            "POCKET_CONQUEST_API_BASE must be https:// or a loopback/private address."
        )}

    err = _validate_game_id(game_id)
    if err:
        return {"error": err}

    # --- fetch current state ---
    game_url = f"{api_base}/pocket-conquest/games/{urllib.parse.quote(game_id, safe='')}.json"
    raw = _http_get(game_url)
    if "error" in raw:
        return raw

    pos_raw = raw.get("position") or raw
    try:
        pos = pc.normalise_position(pos_raw)
    except Exception as exc:  # noqa: BLE001
        return {"error": f"Could not parse game position: {exc}"}

    # --- guard: game already finished ---
    if pc.winner(pos) is not None:
        return {"error": f"Game {game_id} is already finished (winner: {pc.winner(pos)})."}

    # --- guard: malformed move string ---
    # is_legal also checks grammar, but give a targeted message first.
    if not isinstance(move, str) or not move.strip():
        return {"error": "move must be a non-empty string."}

    # --- guard: illegal move ---
    if not pc.is_legal(pos, move):
        return {
            "error": (
                f"Move {move!r} is not legal in this position. "
                "Call list_legal_moves to see valid options."
            )
        }

    # --- compute new position locally (for digest comparison) ---
    try:
        new_pos = pc.apply_move(pos, move)
    except Exception as exc:  # noqa: BLE001
        return {"error": f"Engine error applying move: {exc}"}

    expected_digest = _position_digest(new_pos)

    # --- submit to server ---
    post_url = f"{api_base}/pocket-conquest/api/move"
    post_resp = _http_post(post_url, {
        "game_id": game_id,
        "move": move,
        "position": new_pos,
        # optimistic concurrency: name the state this move was computed from, so
        # the API can refuse a move built on a stale copy instead of overwriting it
        "from_digest": _position_digest(pos_raw),
    })
    if "error" in post_resp:
        return post_resp

    # --- re-fetch to confirm ---
    confirm_raw = _http_get(game_url)
    if "error" in confirm_raw:
        return {
            "error": (
                f"Move was submitted but could not re-fetch game to confirm: "
                f"{confirm_raw['error']}"
            )
        }

    confirm_pos_raw = confirm_raw.get("position") or confirm_raw
    try:
        confirm_pos = pc.normalise_position(confirm_pos_raw)
    except Exception as exc:  # noqa: BLE001
        return {
            "error": (
                f"Move submitted but server returned an unparseable position: {exc}. "
                "Cannot confirm success."
            )
        }

    actual_digest = _position_digest(confirm_pos)
    if actual_digest != expected_digest:
        return {
            "error": (
                "Move was submitted but the server position does not match what the "
                f"engine computed (expected digest {expected_digest}, "
                f"got {actual_digest}). The submission was NOT confirmed."
            )
        }

    # --- build response ---
    win = pc.winner(confirm_pos)
    armies: dict[str, int] = {
        tile: data["armies"]
        for tile, data in confirm_pos["grid"].items()
        if data.get("armies", 0)
    }
    return {
        "game_id": game_id,
        "move": move,
        "status": "finished" if win else "active",
        "side_to_move": confirm_pos["side"],
        "winner": win,
        "board": pc.describe(confirm_pos),
        "armies": armies,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    mcp.run(transport="stdio")
