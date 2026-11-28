"""Pocket Conquest MCP server -- play the game through tools over stdio.

Three tools: get_game, list_legal_moves, make_move. Legality is delegated to the
rules engine in the sibling `pocket-conquest-bridge/` directory (task 0275); this
module only speaks MCP and the HTTP game API.

No credentials, no listener: stdio transport, and the game API carries no auth.
"""

from __future__ import annotations

import hashlib
import ipaddress
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

_HERE = os.path.dirname(os.path.abspath(__file__))
_ENGINE_DIR = os.path.join(os.path.dirname(_HERE), "pocket-conquest-bridge")
if _ENGINE_DIR not in sys.path:
    sys.path.insert(0, _ENGINE_DIR)

import pocket_conquest as pc  # noqa: E402  (path set up above)

try:  # mcp >= 2.0 renamed FastMCP to MCPServer
    from mcp.server.mcpserver import MCPServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as MCPServer  # type: ignore[no-redef]

API_BASE = os.environ.get("POCKET_CONQUEST_API_BASE", "http://127.0.0.1:8791").rstrip("/")
HTTP_TIMEOUT = 10.0
MAX_BODY = 64 * 1024
MAX_MOVE_LEN = 64
GAME_ID_RE = re.compile(r"\A[A-Za-z0-9_-]{1,64}\Z")
_LOOPBACK_NAMES = {"localhost"}

RULES_SUMMARY = (
    "Pocket Conquest: 6x5 grid (columns A-F, rows 1-5), orthogonal movement only. "
    "You are Nordhold (S), the opponent is Ember Court (R); capitals B5 (S) and E1 (R). "
    "Each turn: build (5 gold/army at a capital or owned town; new armies cannot move), "
    "then move/attack one tile per stack, then collect income (capital 5, town 4, plain 2). "
    "Combat: armies + 1d6 vs armies + 1d6 (+1 on hills); ties favour the defender; the loser "
    "loses 1 army, or 2 if the margin is 3+. A tile falls only when its defenders reach zero. "
    "An adjacent friendly stack may join one attack (pincer). Win by occupying the enemy capital. "
    "Moves: 'B <tile> <n>', 'M <src> <dst> <n>', 'M <src> <dst> <n> + <src2> <n2>', 'END' "
    "(END collects income and hands the turn over). list_legal_moves is authoritative."
)

mcp = MCPServer("pocket-conquest")


class ApiError(Exception):
    """A failure talking to the game API. Never escapes a tool call."""


def _base_is_allowed(base: str) -> bool:
    """https anywhere; plain http only for loopback / private test bases."""
    parts = urllib.parse.urlsplit(base)
    if parts.scheme == "https":
        return True
    if parts.scheme != "http":
        return False
    host = parts.hostname or ""
    if host in _LOOPBACK_NAMES:
        return True
    try:
        addr = ipaddress.ip_address(host)
    except ValueError:
        return False
    return addr.is_loopback or addr.is_private


def _request(method: str, path: str, payload: dict | None = None) -> dict:
    """JSON round trip against the game API. Raises ApiError only."""
    if not _base_is_allowed(API_BASE):
        raise ApiError("POCKET_CONQUEST_API_BASE must be https (plain http only for loopback)")
    headers = {"Accept": "application/json", "User-Agent": "pocket-conquest-mcp/1.0"}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{API_BASE}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            raw = resp.read(MAX_BODY)
            status = resp.status
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            parsed = json.loads(exc.read(MAX_BODY).decode("utf-8", "replace"))
            if isinstance(parsed, dict):
                detail = str(parsed.get("error") or "")
        except (ValueError, OSError):
            detail = ""
        raise ApiError(f"game API refused the request (HTTP {exc.code}){(': ' + detail) if detail else ''}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise ApiError(f"game API unreachable: {exc.__class__.__name__}") from None
    if status not in (200, 201):
        raise ApiError(f"game API returned unexpected HTTP {status}")
    try:
        parsed = json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        raise ApiError("game API returned non-JSON") from None
    if not isinstance(parsed, dict):
        raise ApiError("game API returned an unexpected shape")
    return parsed


def _check_game_id(game_id: object) -> str:
    if not isinstance(game_id, str) or not GAME_ID_RE.match(game_id):
        raise ApiError("game_id must be 1-64 chars of [A-Za-z0-9_-]")
    return game_id


def _fetch(game_id: str) -> dict:
    return _request("GET", f"/pocket-conquest/games/{_check_game_id(game_id)}.json")


def _create() -> dict:
    return _request("POST", "/pocket-conquest/api/new", {})


def _digest(position: object) -> str:
    blob = json.dumps(position, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def _position_of(game: dict) -> dict:
    position = game.get("position")
    if not isinstance(position, dict):
        raise ApiError("game carries no position")
    try:
        return pc.normalise_position(position)
    except (ValueError, KeyError, TypeError):
        raise ApiError("game carries an unplayable position") from None


def _side_to_move(game: dict, position: dict) -> str | None:
    status = game.get("status")
    if status == "finished" or game.get("winner") in ("S", "R") or pc.winner(position) is not None:
        return None
    return position["side"]


def _armies(position: dict) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for tile, cell in position["grid"].items():
        if cell.get("armies", 0) or cell.get("owner"):
            entry = {"side": cell.get("owner"), "armies": cell.get("armies", 0)}
            if cell.get("town"):
                entry["town"] = cell["town"]["name"]
                entry["militia"] = cell["town"]["militia"]
            out[tile] = entry
    return out


def _summarise(game: dict, position: dict) -> dict:
    return {
        "game_id": game.get("game_id"),
        "status": "finished" if pc.winner(position) is not None else "active",
        "side_to_move": _side_to_move(game, position),
        "winner": pc.winner(position),
        "turn": position.get("turn"),
        "gold": position.get("gold"),
        "armies": _armies(position),
        "board": pc.describe(position),
        "legal_moves": pc.legal_moves(position),
        "rules": RULES_SUMMARY,
    }


@mcp.tool()
def get_game(game_id: str = "") -> dict:
    """Read a Pocket Conquest game. Call with no game_id to CREATE a new game.

    Pocket Conquest is a 6x5 board game (columns A-F, rows 1-5), orthogonal moves only.
    You are Nordhold ("S"); the opponent is Ember Court ("R"). Capitals: B5 = yours,
    E1 = theirs. A turn is build -> move/attack -> income; armies cost 5 gold, income is
    capital 5 / town 4 / plain 2; combat is armies + 1d6 with ties to the defender and
    +1 for a defender on hills. Win by occupying E1.

    Moves: "B <tile> <n>" build, "M <src> <dst> <n>" move/attack,
    "M <src> <dst> <n> + <src2> <n2>" pincer, "END" collect income and pass.

    Returns the board as text, per-tile armies, gold, side to move, the full legal move
    list and the rules summary. Passing a game_id reads that game instead.
    """
    try:
        game = _create() if not game_id else _fetch(_check_game_id(game_id))
        position = _position_of(game)
    except ApiError as exc:
        return {"error": str(exc)}
    return _summarise(game, position)


@mcp.tool()
def list_legal_moves(game_id: str) -> dict:
    """List every legal move for the side to move in a Pocket Conquest game.

    The board is 6x5 (columns A-F, rows 1-5), orthogonal movement only; you play
    Nordhold ("S"). Grammar: "B <tile> <n>" builds n armies at a capital or owned town,
    "M <src> <dst> <n>" moves or attacks one tile with n armies,
    "M <src> <dst> <n> + <src2> <n2>" is a pincer with a second adjacent stack, and
    "END" collects income and ends the turn. This list is authoritative: make_move
    refuses anything that is not in it.
    """
    try:
        game = _fetch(_check_game_id(game_id))
        position = _position_of(game)
    except ApiError as exc:
        return {"error": str(exc)}
    moves = pc.legal_moves(position)
    return {
        "game_id": game.get("game_id"),
        "side_to_move": _side_to_move(game, position),
        "winner": pc.winner(position),
        "moves": moves,
        "count": len(moves),
    }


@mcp.tool()
def make_move(game_id: str, move: str) -> dict:
    """Play one legal move in a Pocket Conquest game and return the new position.

    You are Nordhold ("S") on a 6x5 grid (columns A-F, rows 1-5), orthogonal movement.
    Grammar: "B <tile> <n>" (build, 5 gold per army at a capital or owned town),
    "M <src> <dst> <n>" (move or attack one tile with n armies),
    "M <src> <dst> <n> + <src2> <n2>" (pincer), "END" (collect income, pass the turn).
    Combat is armies + 1d6, ties to the defender, +1 for a defender on hills.

    The move is validated with the rules engine before anything is sent. An illegal
    move, a move for the wrong side, and any move in a finished game are refused with
    a clear error and nothing is stored. Success is only reported after the game is
    re-read and the move is found there.
    """
    try:
        _check_game_id(game_id)
        if not isinstance(move, str) or not move.strip() or len(move) > MAX_MOVE_LEN:
            raise ApiError("move must be a non-empty string of at most 64 chars")
        move = " ".join(move.split())
        game = _fetch(game_id)
        raw = game.get("position")
        position = _position_of(game)
        if pc.winner(position) is not None:
            raise ApiError(f"game is finished (winner {pc.winner(position)})")
        if not pc.is_legal(position, move):
            raise ApiError(
                f"{move!r} is not legal for {position['side']} in this position; "
                "call list_legal_moves for the legal list"
            )
        expected = pc.apply_move(position, move)
        # POST, then re-read: an HTTP 200 is a claim, not evidence that the move landed.
        _request(
            "POST",
            "/pocket-conquest/api/move",
            {"game_id": game_id, "move": move, "position": expected, "from_digest": _digest(raw)},
        )
        after = _fetch(game_id)
        stored = _position_of(after)
        if _digest(stored) != _digest(expected):
            raise ApiError("the move was submitted but the game does not reflect it (not confirmed)")
        summary = _summarise(after, stored)
        summary["move"] = move
        summary.pop("rules", None)
        summary.pop("legal_moves", None)
        summary["legal_moves_count"] = len(pc.legal_moves(stored))
        return summary
    except ApiError as exc:
        return {"error": str(exc)}
    except (ValueError, KeyError, TypeError) as exc:
        return {"error": f"could not apply the move: {exc.__class__.__name__}"}


def main() -> int:
    mcp.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
