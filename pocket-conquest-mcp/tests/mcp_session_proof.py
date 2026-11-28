"""End-to-end proof for the Pocket Conquest MCP server (stdio transport).

Runs the whole session a cold agent would run, against the local stand-in API:

  1. spawn the game API stand-in (loopback, throwaway store)
  2. spawn `server.py` over stdio as an MCP client
  3. get_game("")            -> creates a test game
  4. get_game(<id>)          -> reads the position (board, gold, armies, side to move)
  5. list_legal_moves(<id>)  -> the full legal list
  6. make_move(<id>, legal)  -> accepted, and the move lands in the game JSON on the API side
  7. refusals: illegal move, wrong-side move, finished game -- each a clear error and
     NOTHING written to the game
  8. prints the resulting game JSON and exits non-zero if any check fails.

Run:  <python with mcp> tests/mcp_session_proof.py [--api-port 8791]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from local_api import Store, _digest, serve  # noqa: E402
import pocket_conquest as pc  # noqa: E402

from mcp import ClientSession, StdioServerParameters  # noqa: E402
from mcp.client.stdio import stdio_client  # noqa: E402

CHECKS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}", flush=True)


def get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def payload(result) -> dict:
    """Structured tool payload from an MCP CallToolResult."""
    if getattr(result, "structuredContent", None):
        sc = result.structuredContent
        if isinstance(sc, dict):
            return sc.get("result", sc) if set(sc) == {"result"} else sc
    for block in result.content or []:
        text = getattr(block, "text", None)
        if text:
            try:
                return json.loads(text)
            except ValueError:
                return {"text": text}
    return {}


async def run_session(api_base: str, api_port: int, store_dir: str) -> None:
    env = dict(os.environ)
    env["POCKET_CONQUEST_API_BASE"] = api_base
    params = StdioServerParameters(
        command=sys.executable,
        args=[os.path.join(ROOT, "server.py")],
        env=env,
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            info = getattr(init, "server_info", None) or getattr(init, "serverInfo", None)
            print(f"initialized: server={getattr(info, 'name', '?')} protocol={getattr(init, 'protocol_version', getattr(init, 'protocolVersion', '?'))}", flush=True)
            tools = await session.list_tools()
            names = sorted(t.name for t in tools.tools)
            check("three tools advertised", names == ["get_game", "list_legal_moves", "make_move"], ",".join(names))
            desc = {t.name: (t.description or "") for t in tools.tools}
            check(
                "tool descriptions carry the rules",
                all("Pocket Conquest" in d and ("A-F" in d or "column" in d.lower()) and "B <tile> <n>" in d
                    for d in desc.values()),
                "each description names the game, the grid and the move grammar",
            )

            # --- 3. create a test game -------------------------------------
            created = payload(await session.call_tool("get_game", {}))
            game_id = created.get("game_id")
            check("get_game('') creates a test game", isinstance(game_id, str) and len(game_id) >= 8, str(game_id))
            if not game_id:
                return

            # --- API-side truth for comparisons ---------------------------
            api_game = get_json(f"{api_base}/pocket-conquest/games/{game_id}.json")

            # --- 4. read the position -------------------------------------
            read_back = payload(await session.call_tool("get_game", {"game_id": game_id}))
            check(
                "get_game(id) reports side to move, gold, armies and the board",
                read_back.get("side_to_move") == "S"
                and str(read_back.get("gold", {}).get("S")) == str(api_game["position"]["gold"]["S"])
                and bool(read_back.get("board"))
                and isinstance(read_back.get("armies"), dict),
                f"side_to_move={read_back.get('side_to_move')} gold={read_back.get('gold')}",
            )

            # --- 5. legal moves ------------------------------------------
            listed = payload(await session.call_tool("list_legal_moves", {"game_id": game_id}))
            moves = listed.get("moves") or []
            engine_moves = pc.legal_moves(api_game["position"])
            check(
                "list_legal_moves matches the engine exactly",
                moves == engine_moves and listed.get("count") == len(moves),
                f"{len(moves)} moves, engine {len(engine_moves)}",
            )

            # --- 6. one legal move ---------------------------------------
            legal = next((m for m in moves if m.startswith("M ")), moves[0])
            played = payload(await session.call_tool("make_move", {"game_id": game_id, "move": legal}))
            after = get_json(f"{api_base}/pocket-conquest/games/{game_id}.json")
            moved = after["moves"] == [legal] and _digest(after["position"]) != _digest(api_game["position"])
            check(
                f"make_move({legal!r}) lands in the game JSON",
                moved and played.get("status") == "active" and played.get("side_to_move") == "S",
                f"moves={after['moves']} side_to_move={played.get('side_to_move')} (the move alone "
                "does not hand the turn over; END does)",
            )
            check(
                "the API-side state is the position the engine produced",
                _digest(after["position"]) == _digest(pc.apply_move(api_game["position"], legal)),
                "digest equal",
            )

            # --- 7a. illegal move refused, nothing written ---------------
            before_moves = list(after["moves"])
            illegal = payload(await session.call_tool("make_move", {"game_id": game_id, "move": "M A1 A2 9"}))
            after_illegal = get_json(f"{api_base}/pocket-conquest/games/{game_id}.json")
            check(
                "an illegal move is refused and nothing is stored",
                bool(illegal.get("error")) and after_illegal["moves"] == before_moves,
                f"error={illegal.get('error')!r}",
            )

            # --- hand the turn over, then try a move for the side not to move ---
            ended = payload(await session.call_tool("make_move", {"game_id": game_id, "move": "END"}))
            check(
                "END collects income and passes the turn to R",
                ended.get("side_to_move") == "R" and ended.get("winner") is None,
                f"side_to_move={ended.get('side_to_move')} gold={ended.get('gold')}",
            )
            before_moves = list(get_json(f"{api_base}/pocket-conquest/games/{game_id}.json")["moves"])
            # --- 7b. wrong-side move refused -----------------------------
            # It is R's turn now; a Nordhold stack move is not R's to make.
            wrong_side = payload(await session.call_tool("make_move", {"game_id": game_id, "move": "M B5 B4 1"}))
            after_wrong = get_json(f"{api_base}/pocket-conquest/games/{game_id}.json")
            check(
                "a move for the side not to move is refused",
                bool(wrong_side.get("error")) and after_wrong["moves"] == before_moves,
                f"error={wrong_side.get('error')!r}",
            )

            # --- 7c. finished game refused -------------------------------
            store = Store(store_dir)
            finished = store.create()
            pos = finished["position"]
            pos["grid"]["E1"]["owner"] = "S"           # Nordhold has taken Emberhold
            pos["grid"]["E1"]["armies"] = 1
            finished["position"] = pos
            finished["digest"] = _digest(pos)
            store._write(finished)
            done = payload(await session.call_tool("make_move", {"game_id": finished["game_id"], "move": "END"}))
            after_done = get_json(f"{api_base}/pocket-conquest/games/{finished['game_id']}.json")
            check(
                "a move on a finished game is refused",
                bool(done.get("error")) and after_done["moves"] == [] and after_done["winner"] == "S",
                f"error={done.get('error')!r}",
            )

            # --- malformed input ----------------------------------------
            junk = payload(await session.call_tool("make_move", {"game_id": game_id, "move": "take the town"}))
            bad_id = payload(await session.call_tool("get_game", {"game_id": "../../etc/passwd"}))
            check(
                "malformed move and malformed game id are refused",
                bool(junk.get("error")) and bool(bad_id.get("error")),
                f"move_error={junk.get('error')!r} id_error={bad_id.get('error')!r}",
            )

            print("\n--- final game JSON (API side) ---", flush=True)
            print(json.dumps(after_wrong, indent=2)[:2500], flush=True)
            print(f"\ntest game id: {game_id} (store {store_dir}, delete the file to remove it)", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--api-port", type=int, default=8791)
    ap.add_argument("--store", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "pc-proof-games"))
    args = ap.parse_args()

    import shutil

    shutil.rmtree(args.store, ignore_errors=True)
    httpd = serve("127.0.0.1", args.api_port, args.store)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.2)
    api_base = f"http://127.0.0.1:{args.api_port}"
    try:
        asyncio.run(run_session(api_base, args.api_port, args.store))
    finally:
        httpd.shutdown()
        httpd.server_close()

    failed = [name for name, ok, _ in CHECKS if not ok]
    print(f"\n{len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed", flush=True)
    if failed:
        print("FAILED: " + "; ".join(failed), flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
