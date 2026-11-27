#!/usr/bin/env python3
"""chess-bridge — a chess-move endpoint for the nordtronics.io chess page.

POST /chess-move  {"fen": "...", "moves": ["e2e4", ...]}  ->  {"move": "<uci>"}

One short DeepSeek completion per move, validated with python-chess; a single
retry carries the error back to the model; Stockfish is the server-side
fallback. Any failure that cannot be covered returns HTTP 500 so the page
falls back to its built-in computer.

The DeepSeek key is read from the environment (DEEPSEEK_API_KEY) and is never
written to a file, logged, or echoed in a response.
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import chess
import chess.engine

BIND = "127.0.0.1"
PORT = int(os.environ.get("CHESS_BRIDGE_PORT", "8765"))

DEEPSEEK_URL = os.environ.get("CHESS_BRIDGE_URL", "https://api.deepseek.com/v1/chat/completions")
MODEL = os.environ.get("CHESS_BRIDGE_MODEL", "deepseek-flash")
STOCKFISH = os.environ.get("CHESS_BRIDGE_STOCKFISH", "/home/astroboy/.local/bin/stockfish")

MODEL_TIMEOUT = 4.0          # per HTTP attempt, seconds
STOCKFISH_MOVETIME_MS = 400
STOCKFISH_TIMEOUT = 3.0
HANDLER_BUDGET = 5.5         # whole request, seconds (page aborts at 6 s)

MAX_BODY = 8192
MAX_FEN = 128
MAX_MOVES = 512
MAX_MOVE_LEN = 5

ALLOWED_ORIGINS = ("https://nordtronics.io", "https://www.nordtronics.io")
DEFAULT_ORIGIN = "https://nordtronics.io"

UCI_RE = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")
TOKEN_RE = re.compile(r"\b([a-h][1-8][a-h][1-8][qrbn]?)\b")

SYSTEM_PROMPT = (
    "You are a chess engine. Reply with exactly one move in UCI notation "
    "(e.g. e2e4, g1f3, e7e8q) taken from the legal move list you are given, "
    "and nothing else. No prose, no move numbers, no punctuation."
)


def engine_version():
    """The engine's own `id name`, or None when it cannot start."""
    if not os.path.isfile(STOCKFISH) or not os.access(STOCKFISH, os.X_OK):
        return None
    try:
        with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as engine:
            return (engine.id or {}).get("name") or "unknown"
    except Exception:
        return None


class BadRequest(Exception):
    pass


def log(status, source, started, extra=""):
    ms = int((time.monotonic() - started) * 1000)
    sys.stderr.write("chess-move status=%s source=%s elapsed_ms=%d%s\n" % (status, source, ms, extra))
    sys.stderr.flush()


def cors_headers(origin):
    allow = origin if origin in ALLOWED_ORIGINS else DEFAULT_ORIGIN
    return {
        "Access-Control-Allow-Origin": allow,
        "Access-Control-Allow-Methods": "POST, OPTIONS",
        "Access-Control-Allow-Headers": "content-type",
        "Access-Control-Max-Age": "600",
        "Vary": "Origin",
    }


def legal_moves(board):
    return sorted(m.uci() for m in board.legal_moves)


def ask_model(fen, moves, legal, prior_error=None):
    """One DeepSeek completion. Returns (move_or_None, error_text, usage)."""
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        return None, "no DEEPSEEK_API_KEY in the service environment", None

    parts = ["FEN: %s" % fen]
    parts.append("Moves so far (UCI): %s" % (" ".join(moves) if moves else "(none)"))
    parts.append("Legal moves (UCI): %s" % " ".join(legal))
    if prior_error:
        parts.append("Your previous answer was rejected: %s" % prior_error)
    parts.append("Reply with one legal move from the list, UCI only.")
    body = {
        "model": MODEL,
        "thinking": {"type": "disabled"},
        "temperature": 0,
        "max_tokens": 16,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": "\n".join(parts)},
        ],
    }
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        DEEPSEEK_URL,
        data=data,
        headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=MODEL_TIMEOUT) as resp:
            payload = json.loads(resp.read(65536).decode("utf-8", "replace"))
    except urllib.error.HTTPError as exc:
        # Never surface the key or the request body; the status is enough.
        return None, "model HTTP %s" % exc.code, None
    except Exception as exc:  # timeout, DNS, TLS, bad JSON
        return None, "model call failed: %s" % type(exc).__name__, None

    usage = payload.get("usage") if isinstance(payload, dict) else None
    try:
        text = payload["choices"][0]["message"]["content"] or ""
    except Exception:
        return None, "model reply had no content", usage
    match = TOKEN_RE.search(text)
    if not match:
        return None, "model reply contained no UCI move (%r)" % text[:40], usage
    candidate = match.group(1)
    if candidate not in legal:
        return None, "model answered %s, which is not in the legal list" % candidate, usage
    return candidate, None, usage


def stockfish_move(fen, moves, legal):
    """Stockfish's best move for the position, or None.

    Driven through python-chess's UCI wrapper: a hand-rolled stdin script that
    writes `quit` straight after `go` cuts the search off at depth 1, which
    yields a legal but tactically blind move (it missed mate in 1).
    """
    if not os.path.isfile(STOCKFISH) or not os.access(STOCKFISH, os.X_OK):
        return None, "stockfish not executable at %s" % STOCKFISH
    board = chess.Board(fen)
    for uci in moves:
        try:
            board.push_uci(uci)
        except ValueError:
            board = chess.Board(fen)  # history is advisory; keep the FEN
            break
    if board.fen() != chess.Board(fen).fen():
        board = chess.Board(fen)
    try:
        with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as engine:
            engine.configure({"Threads": 1, "Hash": 16})
            result = engine.play(board, chess.engine.Limit(time=STOCKFISH_MOVETIME_MS / 1000.0))
    except Exception as exc:
        return None, "stockfish failed: %s" % type(exc).__name__
    if result.move is None:
        return None, "stockfish returned no move"
    best = result.move.uci()
    if best not in legal:
        return None, "stockfish bestmove %s is not legal" % best
    return best, None


def parse_request(raw):
    try:
        doc = json.loads(raw.decode("utf-8"))
    except Exception:
        raise BadRequest("body is not JSON")
    if not isinstance(doc, dict):
        raise BadRequest("body must be a JSON object")
    fen = doc.get("fen")
    moves = doc.get("moves", [])
    if not isinstance(fen, str) or not fen or len(fen) > MAX_FEN:
        raise BadRequest("fen must be a non-empty string of at most %d chars" % MAX_FEN)
    if not isinstance(moves, list) or len(moves) > MAX_MOVES:
        raise BadRequest("moves must be a list of at most %d entries" % MAX_MOVES)
    clean = []
    for item in moves:
        if not isinstance(item, str) or len(item) > MAX_MOVE_LEN:
            raise BadRequest("every move must be a short UCI string")
        clean.append(item)
    try:
        board = chess.Board(fen)
    except Exception:
        raise BadRequest("fen is not a valid position")
    if not board.is_valid():
        raise BadRequest("fen is not a legal position")
    return fen, clean, board


def pick_move(fen, moves, board, started):
    legal = legal_moves(board)
    if not legal:
        raise BadRequest("the position has no legal moves")

    move, err, usage = ask_model(fen, moves, legal)
    if move is None and time.monotonic() - started < HANDLER_BUDGET - MODEL_TIMEOUT:
        move, err2, usage2 = ask_model(fen, moves, legal, prior_error=err)
        if move is None:
            err = err2 or err

    if move is not None:
        return move, "model", usage

    fallback, ferr = stockfish_move(fen, moves, legal)
    if fallback is not None:
        return fallback, "stockfish", usage
    raise RuntimeError("model: %s; stockfish: %s" % (err or "failed", ferr or "failed"))


class Handler(BaseHTTPRequestHandler):
    server_version = "chess-bridge/1.0"
    protocol_version = "HTTP/1.1"

    def _send(self, status, payload, origin, extra=None):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for key, value in cors_headers(origin).items():
            self.send_header(key, value)
        if extra:
            for key, value in extra.items():
                self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        if self.path.split("?")[0] not in ("/chess-move", "/"):
            self._send(404, {"error": "not found"}, self.headers.get("Origin", ""))
            return
        self.send_response(204)
        for key, value in cors_headers(self.headers.get("Origin", "")).items():
            self.send_header(key, value)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        if self.path.split("?")[0] == "/health":
            self._send(200, {"ok": True, "model": MODEL, "stockfish": ENGINE_NAME},
                       self.headers.get("Origin", ""))
            return
        self._send(404, {"error": "not found"}, self.headers.get("Origin", ""))

    def do_POST(self):
        started = time.monotonic()
        origin = self.headers.get("Origin", "")
        if self.path.split("?")[0] not in ("/chess-move", "/"):
            self._send(404, {"error": "not found"}, origin)
            log(404, "none", started)
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_BODY:
            self._send(400, {"error": "missing or oversized body"}, origin)
            log(400, "none", started)
            return
        try:
            raw = self.rfile.read(length)
            fen, moves, board = parse_request(raw)
            move, source, usage = pick_move(fen, moves, board, started)
        except BadRequest as exc:
            self._send(400, {"error": str(exc)}, origin)
            log(400, "none", started)
            return
        except Exception as exc:
            self._send(500, {"error": str(exc)[:200]}, origin)
            log(500, "none", started)
            return
        self._send(200, {"move": move}, origin)
        log(200, source, started, " usage=%s" % json.dumps(usage) if usage else "")

    def log_message(self, fmt, *args):  # silence the default stderr access log
        return


ENGINE_NAME = None


def main():
    global ENGINE_NAME
    if not os.environ.get("DEEPSEEK_API_KEY"):
        sys.stderr.write("chess-bridge: warning: DEEPSEEK_API_KEY is not set; "
                         "every move will fall back to Stockfish\n")
    ENGINE_NAME = engine_version()
    if ENGINE_NAME is None:
        sys.stderr.write("chess-bridge: warning: no working Stockfish at %s; "
                         "model failures become HTTP 500\n" % STOCKFISH)
    elif not ENGINE_NAME.startswith("Stockfish 17.1"):
        sys.stderr.write("chess-bridge: warning: Stockfish reports %r; "
                         "this build is pinned to Stockfish 17.1\n" % ENGINE_NAME)
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write("chess-bridge: listening on http://%s:%d (model=%s, engine=%s)\n"
                     % (BIND, PORT, MODEL, ENGINE_NAME))
    sys.stderr.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
