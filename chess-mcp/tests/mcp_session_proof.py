"""End-to-end proof for the chess MCP server.

Spawns chess-mcp/server.py over stdio with the real MCP client, plays two moves
through the protocol (with both refusals exercised first), and re-reads the
public game JSON. Prints a transcript to stdout and the game id at the end.

Run with the venv that has mcp + chess:
  /home/astroboy/.local/share/chess-mcp/venv/bin/python tests/mcp_session_proof.py

The game API has no delete endpoint, so each fresh run leaves one public game
behind; pass --game <8-char-id> to reuse an existing game instead.
"""

from __future__ import annotations

import asyncio
import json
import os
import secrets
import sys
import urllib.request

from mcp import ClientSession, StdioServerParameters, stdio_client

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(os.path.dirname(HERE), "server.py")
BASE = os.environ.get("CHESS_API_BASE", "https://nordtronics.io/chess").rstrip("/")


def post(path: str, payload: dict) -> dict:
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "User-Agent": "chess-mcp-proof"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read(65536).decode())


def get(path: str) -> dict:
    req = urllib.request.Request(BASE + path, headers={"User-Agent": "chess-mcp-proof"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read(65536).decode())


def payload_of(result) -> dict:
    if getattr(result, "structuredContent", None):
        return result.structuredContent
    if result.content:
        text = getattr(result.content[0], "text", "")
        try:
            return json.loads(text)
        except ValueError:
            return {"raw": text}
    return {}


def show(label: str, result) -> dict:
    body = payload_of(result)
    print(f"\n$ {label}\n{json.dumps(body, sort_keys=True)}")
    return body


def illegal_uci(legal: list[str]) -> str:
    for candidate in ("e2e5", "a7a2", "h8h1", "b1b1"):
        if candidate not in legal:
            return candidate
    raise SystemExit("no illegal-looking UCI candidate available")


async def main(reuse: str | None) -> int:
    if reuse:
        game_id = reuse
        print(f"reusing game: {json.dumps(get('/games/' + game_id + '.json'))}")
    else:
        # the web page uses 8-char game codes; match that so the page can load it
        rng = "abcdefghijkmnpqrstuvwxyz23456789"
        game_id = "mcp" + "".join(secrets.choice(rng) for _ in range(5))
        print(f"created game: {json.dumps(post('/api/new', {'game_id': game_id, 'juno_side': 'black'}))}")

    params = StdioServerParameters(command=sys.executable, args=[SERVER], env=dict(os.environ))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            info = await session.initialize()
            print(f"\nserver: {info.server_info.name} {info.server_info.version}")
            tools = await session.list_tools()
            print("tools: " + ", ".join(t.name for t in tools.tools))

            start = show(f"get_game({game_id})", await session.call_tool("get_game", {"game_id": game_id}))
            base_n = len(start["moves"])
            if base_n == 0:
                assert start["side_to_move"] == "white", start
                assert start["status"] == "active", start

            legal = show(
                f"list_legal_moves(game_id={game_id})",
                await session.call_tool("list_legal_moves", {"game_id": game_id}),
            )
            assert legal["moves"], legal
            wrong_turn = await session.call_tool(
                "make_move",
                {"game_id": game_id, "move": legal["moves"][0], "side": "white" if start["side_to_move"] == "black" else "black"},
            )

            first = legal["moves"][0]
            show(
                f"make_move illegal ({illegal_uci(legal['moves'])})",
                await session.call_tool("make_move", {"game_id": game_id, "move": illegal_uci(legal["moves"])}),
            )
            show(f"make_move wrong turn (side=opposite, {first})", wrong_turn)
            assert "error" in payload_of(wrong_turn), "wrong-turn move was not refused"
            falsey = await session.call_tool(
                "make_move", {"game_id": game_id, "move": first, "side": 0}
            )
            show("make_move non-string side (0)", falsey)
            # the SDK's own schema check may reject it first (isError) or our
            # in-function type check may answer with {"error": ...}
            assert falsey.is_error or "error" in payload_of(falsey), "a non-string side was accepted"

            # the refusals must have submitted nothing
            unchanged = show(
                "get_game -> still at the starting move count",
                await session.call_tool("get_game", {"game_id": game_id}),
            )
            assert len(unchanged["moves"]) == base_n, unchanged

            one = show(
                f"make_move({game_id}, {first})",
                await session.call_tool("make_move", {"game_id": game_id, "move": first}),
            )
            assert one.get("error") is None, one

            after_one = show(
                "get_game -> position advanced, list_legal_moves now offers the other side",
                await session.call_tool("get_game", {"game_id": game_id}),
            )
            assert len(after_one["moves"]) == base_n + 1, after_one

            second_legal = payload_of(
                await session.call_tool("list_legal_moves", {"game_id": game_id})
            )["moves"]
            two = show(
                f"make_move({game_id}, {second_legal[0]})",
                await session.call_tool("make_move", {"game_id": game_id, "move": second_legal[0]}),
            )
            assert two.get("error") is None, two

    public = get(f"/games/{game_id}.json")
    print(f"\n$ GET /games/{game_id}.json\n{json.dumps({k: public[k] for k in ('id', 'moves', 'fen', 'status')})}")
    assert len(public["moves"]) == base_n + 2, public
    print(f"\nGAME_ID={game_id}  (no delete endpoint on the game API: remove it server-side)")
    return 0


if __name__ == "__main__":
    if "--game" in sys.argv[1:]:
        if len(sys.argv) < 3 or not sys.argv[2]:
            raise SystemExit("--game needs the 8-character game id it should reuse")
        reuse = sys.argv[2]
    else:
        reuse = None
    raise SystemExit(asyncio.run(main(reuse)))
