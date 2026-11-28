"""Local stand-in for the Pocket Conquest game API (test harness, stdlib only).

The live API on the VPS is Juno's to stand up; this process implements the same
documented contract so the MCP server can be proved end to end without it:

  POST /pocket-conquest/api/new            -> create a game
  POST /pocket-conquest/api/move           -> submit one validated turn
  GET  /pocket-conquest/games/<id>.json    -> read the current game

Shapes (JSON in and out):

  new      request  : {} (an empty body is also accepted)
           response : {"game_id", "status", "side_to_move", "winner", "position"}
  move     request  : {"game_id", "move", "from_digest"}
           response : {"game_id", "move", "status", "side_to_move", "winner",
                       "position", "accepted": true}
           409      : {"error": ...} -- illegal move, wrong turn, stale position,
                       finished game, or unknown id. Nothing is stored.
  games    response : {"game_id", "status", "side_to_move", "winner", "position",
                       "moves"}

Unlike the MCP server, this stand-in is authoritative on purpose: it re-validates
every submitted move through the rules engine, so a bug in the client cannot make
the proof pass. It is bound to loopback only and stores games as JSON files.

Run:  python3 local_api.py --port 8791 --store /tmp/pc-games
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pocket-conquest-bridge"))

import pocket_conquest as pc  # noqa: E402

MAX_BODY = 64 * 1024
GAME_ID_RE = re.compile(r"\A[A-Za-z0-9_-]{1,64}\Z")
NEW_PATH = "/pocket-conquest/api/new"
MOVE_PATH = "/pocket-conquest/api/move"
GAME_PATH_RE = re.compile(r"\A/pocket-conquest/games/([A-Za-z0-9_-]{1,64})\.json\Z")

_LOCK = threading.Lock()


def _digest(position: dict) -> str:
    """Stable digest of a position, used to detect a stale client copy."""
    blob = json.dumps(position, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


class Store:
    """Games as one JSON file each. In-memory would do; files make the proof inspectable."""

    def __init__(self, root: str) -> None:
        self.root = root
        os.makedirs(root, exist_ok=True)

    def _path(self, game_id: str) -> str:
        return os.path.join(self.root, f"{game_id}.json")

    def create(self) -> dict:
        game_id = secrets.token_hex(8)
        position = pc.initial_position()
        game = {
            "game_id": game_id,
            "position": position,
            "moves": [],
            "digest": _digest(position),
        }
        self._write(game)
        return game

    def read(self, game_id: str) -> dict | None:
        path = self._path(game_id)
        if not os.path.exists(path):
            return None
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)

    def _write(self, game: dict) -> None:
        tmp = self._path(game["game_id"]) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(game, fh)
        os.replace(tmp, self._path(game["game_id"]))

    def move(self, game_id: str, move: str, from_digest: object) -> tuple[int, dict]:
        with _LOCK:
            game = self.read(game_id)
            if game is None:
                return 404, {"error": "unknown game"}
            position = game["position"]
            done = pc.winner(position)
            if done is not None:
                return 409, {"error": f"game is finished (winner {done})"}
            # optimistic concurrency: the client names the state its move was computed
            # from, so a second writer's turn cannot silently overwrite this one.
            if not isinstance(from_digest, str) or from_digest != game["digest"]:
                return 409, {"error": "stale game: the move was computed from an older state"}
            if not pc.is_legal(position, move):
                return 409, {"error": f"{move!r} is not legal for {position['side']} in this position"}
            new_position = pc.apply_move(position, move)
            game["position"] = new_position
            game["moves"].append(move)
            game["digest"] = _digest(new_position)
            self._write(game)
            return 200, game


def _public(game: dict) -> dict:
    position = game["position"]
    done = pc.winner(position)
    return {
        "game_id": game["game_id"],
        "status": "finished" if done else "active",
        "side_to_move": None if done else position["side"],
        "winner": done,
        "position": position,
        "moves": list(game["moves"]),
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "pocket-conquest-standin/1.0"
    store: Store

    def log_message(self, format: str, *args) -> None:  # noqa: A002,A003 - base signature
        if os.environ.get("PC_STANDIN_VERBOSE") == "1":
            sys.stderr.write("%s - %s\n" % (self.address_string(), format % args))

    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> tuple[bool, object]:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return False, "bad Content-Length"
        if length < 0 or length > MAX_BODY:
            return False, "body too large"
        raw = self.rfile.read(length) if length else b"{}"
        if not raw.strip():
            return True, {}
        try:
            return True, json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return False, "body is not JSON"

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path == NEW_PATH:
            game = self.store.create()
            self._send(201, _public(game))
            return
        if self.path == MOVE_PATH:
            ok, payload = self._read_json()
            if not ok:
                self._send(400, {"error": str(payload)})
                return
            if not isinstance(payload, dict):
                self._send(400, {"error": "body must be a JSON object"})
                return
            game_id = payload.get("game_id")
            move = payload.get("move")
            if not isinstance(game_id, str) or not GAME_ID_RE.match(game_id):
                self._send(400, {"error": "game_id must be 1-64 chars of [A-Za-z0-9_-]"})
                return
            if not isinstance(move, str) or len(move) > 64:
                self._send(400, {"error": "move must be a string of at most 64 chars"})
                return
            code, result = self.store.move(game_id, move, payload.get("from_digest"))
            if code == 200:
                out = _public(result)
                out["accepted"] = True
                self._send(200, out)
            else:
                self._send(code, result)
            return
        self._send(404, {"error": "not found"})

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        match = GAME_PATH_RE.match(self.path)
        if match:
            game = self.store.read(match.group(1))
            if game is None:
                self._send(404, {"error": "unknown game"})
                return
            self._send(200, _public(game))
            return
        if self.path == "/health":
            self._send(200, {"ok": True})
            return
        self._send(404, {"error": "not found"})


def serve(host: str = "127.0.0.1", port: int = 8791, store: str = "/tmp/pc-games") -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"store": Store(store)})
    httpd = ThreadingHTTPServer((host, port), handler)
    return httpd


def main() -> int:
    ap = argparse.ArgumentParser(description="Pocket Conquest game API stand-in")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--store", default="/tmp/pc-games")
    args = ap.parse_args()
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        print("refusing to bind a non-loopback address: this is a test harness", file=sys.stderr)
        return 2
    httpd = serve(args.host, args.port, args.store)
    print(f"pocket-conquest stand-in on http://{args.host}:{args.port} store={args.store}", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
