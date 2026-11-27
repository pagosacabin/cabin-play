"""Chess MCP server — lets an MCP client (Hermes) read and play a game on
https://nordtronics.io/chess over stdio.

Tools: get_game, list_legal_moves, make_move.

No credentials, no listener: the public game API carries no auth and the
transport is stdio (the MCP client spawns this process).
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request

import chess

try:  # mcp >= 2.0 renamed FastMCP to MCPServer
    from mcp.server.mcpserver import MCPServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as MCPServer  # type: ignore[no-redef]

API_BASE = os.environ.get("CHESS_API_BASE", "https://nordtronics.io/chess").rstrip("/")
HTTP_TIMEOUT = 10.0
MAX_BODY = 64 * 1024
MAX_FEN = 128

GAME_ID_RE = re.compile(r"\A[A-Za-z0-9_-]{1,64}\Z")
UCI_RE = re.compile(r"\A[a-h][1-8][a-h][1-8][qrbn]?\Z")

mcp = MCPServer("chess")


class ChessApiError(Exception):
    """A failure talking to the game API. Never escapes a tool call."""


def _request(path: str, payload: dict | None = None) -> dict:
    """JSON round trip against the game API. Raises ChessApiError only."""
    url = f"{API_BASE}{path}"
    data = None
    headers = {"Accept": "application/json", "User-Agent": "chess-mcp/1.0"}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            raw = resp.read(MAX_BODY)
            status = resp.status
    except urllib.error.HTTPError as exc:  # 409 / 404 / 5xx
        body = ""
        try:
            body = exc.read(MAX_BODY).decode("utf-8", "replace")
        except Exception:  # noqa: BLE001 - body already best effort
            pass
        detail = ""
        try:
            parsed = json.loads(body)
            if isinstance(parsed, dict):
                detail = str(parsed.get("error") or "")
        except ValueError:
            detail = ""
        if exc.code == 409:
            raise ChessApiError(detail or "game refused the move (409)") from None
        raise ChessApiError(f"HTTP {exc.code}{(': ' + detail) if detail else ''}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise ChessApiError(f"network error: {exc.__class__.__name__}") from None
    if status != 200:
        raise ChessApiError(f"unexpected HTTP {status}")
    try:
        parsed = json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        raise ChessApiError("game API returned non-JSON") from None
    if not isinstance(parsed, dict):
        raise ChessApiError("game API returned an unexpected shape")
    return parsed


def _check_game_id(game_id: str) -> str:
    if not isinstance(game_id, str) or not GAME_ID_RE.match(game_id):
        raise ChessApiError("game_id must be 1-64 chars of [A-Za-z0-9_-]")
    return game_id


def _fetch_game(game_id: str) -> dict:
    return _request(f"/games/{_check_game_id(game_id)}.json")


def _replay(moves: object) -> chess.Board:
    """Rebuild the position from the SAN move list.

    The API stores whatever `fen` the client posted, and the web page posts
    the position *after* the move. It is still not treated as authoritative:
    replaying the SAN list is what the tools report.
    """
    board = chess.Board()
    if moves is None:
        return board
    if not isinstance(moves, list) or len(moves) > 512:
        raise ChessApiError("game carries an unplayable move list")
    for san in moves:
        if not isinstance(san, str) or len(san) > 8:
            raise ChessApiError("game carries a malformed move")
        try:
            board.push_san(san)
        except ValueError:
            raise ChessApiError(f"game carries an illegal move: {san}") from None
    return board


def _side_name(board: chess.Board) -> str:
    return "white" if board.turn == chess.WHITE else "black"


def _status_of(game: dict) -> str:
    status = game.get("status")
    return status if isinstance(status, str) else "unknown"


@mcp.tool()
def get_game(game_id: str) -> dict:
    """Read a game: FEN, SAN move list, side to move and status."""
    try:
        game = _fetch_game(game_id)
        board = _replay(game.get("moves"))
    except ChessApiError as exc:
        return {"error": str(exc)}
    status = _status_of(game)
    return {
        "game_id": game_id,
        "fen": board.fen(),
        "moves": [m for m in (game.get("moves") or []) if isinstance(m, str)],
        "side_to_move": None if status != "active" else _side_name(board),
        "status": status,
        "juno_side": game.get("juno_side") if isinstance(game.get("juno_side"), str) else None,
        # what the API stored (the client-posted fen), for transparency only
        "api_fen": game.get("fen") if isinstance(game.get("fen"), str) else None,
    }


@mcp.tool()
def list_legal_moves(game_id: str = "", fen: str = "") -> dict:
    """Legal moves (UCI) for a game's current position, or for a raw FEN."""
    try:
        if fen:
            if len(fen) > MAX_FEN:
                raise ChessApiError("fen too long (max 128 chars)")
            try:
                board = chess.Board(fen)
            except ValueError:
                raise ChessApiError("fen is not a legal position") from None
            if not board.is_valid():
                raise ChessApiError("fen is not a legal position")
        elif game_id:
            board = _replay(_fetch_game(game_id).get("moves"))
        else:
            raise ChessApiError("pass game_id or fen")
    except ChessApiError as exc:
        return {"error": str(exc)}
    return {"fen": board.fen(), "moves": sorted(m.uci() for m in board.legal_moves)}


@mcp.tool()
def make_move(game_id: str, move: str, side: str = "") -> dict:
    """Play one legal UCI move in an active game and return the new position.

    Refuses (submits nothing) an illegal move, a move for the side that is not
    to move, and any move in a game that is not active.
    """
    try:
        _check_game_id(game_id)
        if not isinstance(move, str) or not UCI_RE.match(move):
            raise ChessApiError("move must be UCI like e2e4 or e7e8q")
        game = _fetch_game(game_id)
        status = _status_of(game)
        if status != "active":
            raise ChessApiError(f"game is not active (status: {status})")
        board = _replay(game.get("moves"))
        to_move = _side_name(board)
        if side and side.lower() not in ("white", "black"):
            raise ChessApiError("side must be white or black when given")
        if side and side.lower() != to_move:
            raise ChessApiError(f"not {side.lower()}'s turn ({to_move} to move)")
        try:
            played = chess.Move.from_uci(move)
        except ValueError:
            raise ChessApiError("move is not a legal UCI string") from None
        if played not in board.legal_moves:
            raise ChessApiError(f"{move} is not legal in this position")
        san = board.san(played)
        board.push(played)
        # the page posts toFEN(state) *after* the move and its resume path
        # compares the stored fen against the replayed position, so the fen we
        # send must be the post-move one, not the position the move came from
        _request(
            "/api/move",
            {"game_id": game_id, "move": san, "fen": board.fen(), "side": to_move},
        )
    except ChessApiError as exc:
        return {"error": str(exc)}
    return {
        "game_id": game_id,
        "move_uci": move,
        "san": san,
        "fen": board.fen(),
        "side": to_move,
    }


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
