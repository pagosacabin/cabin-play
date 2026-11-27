"""End-to-end proof for the chess MCP server.

Spawns chess-mcp/server.py over stdio with the real MCP client, creates a
throwaway game on the public API, and drives all three tools through the
protocol. Prints a transcript to stdout.

Run with the venv that has mcp + chess:
  /home/astroboy/.local/share/chess-mcp/venv/bin/python tests/mcp_session_proof.py
"""

from __future__ import annotations

import asyncio
import json
import os
import secrets
import sys
import time
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


def show(label: str, result) -> dict:
    payload = {}
    if getattr(result, "structuredContent", None):
        payload = result.structuredContent
    elif result.content:
        text = getattr(result.content[0], "text", "")
        try:
            payload = json.loads(text)
        except ValueError:
            payload = {"raw": text}
    print(f"\n$ {label}\n{json.dumps(payload, sort_keys=True)}")
    return payload


async def main() -> int:
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

            state = show(f"get_game({game_id})", await session.call_tool("get_game", {"game_id": game_id}))
            assert state["side_to_move"] == "white", state

            legal = show(
                f"list_legal_moves(game_id={game_id})",
                await session.call_tool("list_legal_moves", {"game_id": game_id}),
            )
            assert "e2e4" in legal["moves"], legal

            # refusals, all of which must submit nothing
            show(
                "make_move illegal (e2e5)",
                await session.call_tool("make_move", {"game_id": game_id, "move": "e2e5"}),
            )
            show(
                "make_move wrong turn (side=black on move 1)",
                await session.call_tool("make_move", {"game_id": game_id, "move": "e7e5", "side": "black"}),
            )

            played = show(
                f"make_move({game_id}, e2e4)",
                await session.call_tool("make_move", {"game_id": game_id, "move": "e2e4"}),
            )
            assert played["san"] == "e4", played

            after = show(f"get_game({game_id}) after e4", await session.call_tool("get_game", {"game_id": game_id}))
            assert after["moves"] == ["e4"] and after["side_to_move"] == "black", after

            # second move, black, to show both colours flow through
            show(
                f"make_move({game_id}, g8f6)",
                await session.call_tool("make_move", {"game_id": game_id, "move": "g8f6"}),
            )

    public = get(f"/games/{game_id}.json")
    print(f"\n$ GET /games/{game_id}.json\n{json.dumps({k: public[k] for k in ('id', 'moves', 'status')})}")
    assert public["moves"][:2] == ["e4", "Nf6"], public
    print(f"\nGAME_ID={game_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
