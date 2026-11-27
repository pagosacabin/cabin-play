#!/usr/bin/env python3
"""chess-bridge — a chess-move endpoint for the nordtronics.io chess page.

POST /chess-move  {"fen": "...", "moves": ["e2e4", ...]}  ->  {"move": "<uci>"}

One short DeepSeek completion per move, validated with python-chess; a single
retry carries the error back to the model; Stockfish is the server-side
fallback. Any failure that cannot be covered returns HTTP 500 so the page
falls back to its built-in computer.

The DeepSeek key is read from the environment (DEEPSEEK_API_KEY) and is never
written to a file, logged, or echoed in a response.

Hardening (review pass 1, findings 2-12):
  * upstream URL scheme allowlist — the bearer key never travels over cleartext
    unless the target is loopback;
  * a global token bucket and a bounded worker pool, so an unauthenticated
    caller cannot run up the API bill or exhaust threads;
  * one deadline for the whole handler (model attempt, retry, Stockfish all
    draw from it), and the engine is limited by the remaining time;
  * CORS fails closed: an origin outside the allowlist gets no ACAO header and
    a 403 on preflight (no more substituting a trusted origin);
  * move history is UCI-syntax checked at parse time;
  * /health is opaque, 500s carry a stable message, and the details go to the
    journal;
  * header block size and socket read timeout are bounded;
  * a missing engine is fatal at startup, and a swapped engine is refused at
    use time.
"""

import json
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
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

# Concurrency and cost controls. The endpoint is unauthenticated by design
# (the page cannot log in), so the only bound on spend is here.
MAX_CONCURRENCY = int(os.environ.get("CHESS_BRIDGE_MAX_CONCURRENCY", "4"))
SLOTS = threading.BoundedSemaphore(MAX_CONCURRENCY)
SLOT_WAIT = 0.25             # seconds to wait for a free worker before 503
RATE_LIMIT_PER_MIN = int(os.environ.get("CHESS_BRIDGE_RATE_PER_MIN", "60"))
RATE_BURST = int(os.environ.get("CHESS_BRIDGE_RATE_BURST", "10"))

HEADER_TIMEOUT = 5.0         # socket read timeout: bounds a slow-drip client
MAX_HEADER_BYTES = 8192

ALLOWED_ORIGINS = ("https://nordtronics.io", "https://www.nordtronics.io")

UCI_RE = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")
TOKEN_RE = re.compile(r"\b([a-h][1-8][a-h][1-8][qrbn]?)\b")

SYSTEM_PROMPT = (
    "You are a chess engine. Reply with exactly one move in UCI notation "
    "(e.g. e2e4, g1f3, e7e8q) taken from the legal move list you are given, "
    "and nothing else. No prose, no move numbers, no punctuation."
)


def check_upstream_url(url):
    """Allowlist the scheme: the key may only leave the box over TLS.

    Plain http is accepted only for a loopback target (a local proxy or stub),
    so an env override cannot ship the bearer token in cleartext off-box.
    """
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme == "https":
        return
    host = (parsed.hostname or "").lower()
    if parsed.scheme == "http" and host in ("127.0.0.1", "::1", "localhost"):
        return
    raise SystemExit("chess-bridge: refusing CHESS_BRIDGE_URL=%r: it must be https, "
                     "or http to a loopback host" % url)


class RateLimiter:
    """Global token bucket: RATE_BURST immediate, RATE_LIMIT_PER_MIN sustained."""

    def __init__(self, per_min, burst):
        self.rate = per_min / 60.0
        self.capacity = float(burst)
        self.tokens = float(burst)
        self.stamp = time.monotonic()
        self.lock = threading.Lock()

    def allow(self):
        with self.lock:
            now = time.monotonic()
            self.tokens = min(self.capacity, self.tokens + (now - self.stamp) * self.rate)
            self.stamp = now
            if self.tokens < 1.0:
                return False
            self.tokens -= 1.0
            return True


LIMITER = RateLimiter(RATE_LIMIT_PER_MIN, RATE_BURST)


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


def log(method, path, status, source, started, extra=""):
    ms = int((time.monotonic() - started) * 1000)
    sys.stderr.write("chess-move method=%s path=%s status=%s source=%s elapsed_ms=%d%s\n"
                     % (method, path, status, source, ms, extra))
    sys.stderr.flush()


def cors_headers(origin):
    """ACAO only for an allowlisted origin — unknown origins get no grant."""
    if origin not in ALLOWED_ORIGINS:
        return {"Vary": "Origin"}
    return {
        "Access-Control-Allow-Origin": origin,
        "Access-Control-Allow-Methods": "POST, OPTIONS",
        "Access-Control-Allow-Headers": "content-type",
        "Access-Control-Max-Age": "600",
        "Vary": "Origin",
    }


def legal_moves(board):
    return sorted(m.uci() for m in board.legal_moves)


def ask_model(fen, moves, legal, deadline, prior_error=None):
    """One DeepSeek completion. Returns (move_or_None, error_text, usage)."""
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        return None, "no DEEPSEEK_API_KEY in the service environment", None
    timeout = min(MODEL_TIMEOUT, deadline - time.monotonic())
    if timeout <= 0.2:
        return None, "no time left for a model call", None

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
        with urllib.request.urlopen(req, timeout=timeout) as resp:
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


def stockfish_move(fen, moves, legal, deadline):
    """Stockfish's best move for the position, or None.

    Driven through python-chess's UCI wrapper: a hand-rolled stdin script that
    writes `quit` straight after `go` cuts the search off at depth 1, which
    yields a legal but tactically blind move (it missed mate in 1).

    The engine is re-identified on every use, so a binary swapped after startup
    is refused rather than trusted, and the search is limited by whatever is
    left of the handler budget.
    """
    if not os.path.isfile(STOCKFISH) or not os.access(STOCKFISH, os.X_OK):
        return None, "stockfish not executable at %s" % STOCKFISH
    remaining = deadline - time.monotonic()
    if remaining <= 0.15:
        return None, "no time left for stockfish"
    budget = min(STOCKFISH_TIMEOUT, remaining)
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
            name = (engine.id or {}).get("name") or ""
            if not name.startswith("Stockfish 17.1"):
                return None, "stockfish identity changed: %r" % name[:40]
            engine.configure({"Threads": 1, "Hash": 16})
            limit = chess.engine.Limit(
                time=min(STOCKFISH_MOVETIME_MS / 1000.0, max(0.05, budget - 0.5)))
            result = engine.play(board, limit)
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
        if not isinstance(item, str) or len(item) > MAX_MOVE_LEN or not UCI_RE.match(item):
            raise BadRequest("every move must be a UCI string such as e2e4")
        clean.append(item)
    try:
        board = chess.Board(fen)
    except Exception:
        raise BadRequest("fen is not a valid position")
    if not board.is_valid():
        raise BadRequest("fen is not a legal position")
    return fen, clean, board


def pick_move(fen, moves, board, started):
    deadline = started + HANDLER_BUDGET
    legal = legal_moves(board)
    if not legal:
        raise BadRequest("the position has no legal moves")

    move, err, usage = ask_model(fen, moves, legal, deadline)
    if move is None and deadline - time.monotonic() > 1.0:
        move, err2, usage2 = ask_model(fen, moves, legal, deadline, prior_error=err)
        if move is None:
            err = err2 or err

    if move is not None:
        return move, "model", usage

    fallback, ferr = stockfish_move(fen, moves, legal, deadline)
    if fallback is not None:
        return fallback, "stockfish", usage
    raise RuntimeError("model: %s; stockfish: %s" % (err or "failed", ferr or "failed"))


class Handler(BaseHTTPRequestHandler):
    server_version = "chess-bridge/1.1"
    protocol_version = "HTTP/1.1"
    timeout = HEADER_TIMEOUT  # a slow-drip client cannot hold a worker forever

    def _origin(self):
        return self.headers.get("Origin", "")

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

    def parse_request(self):
        # Bound the header block before any dispatch: BaseHTTPRequestHandler
        # buffers headers one line at a time with no total cap of its own.
        if not BaseHTTPRequestHandler.parse_request(self):
            return False
        size = sum(len(k) + len(v) for k, v in self.headers.items())
        if size > MAX_HEADER_BYTES:
            self._send(431, {"error": "request headers too large"}, self._origin())
            return False
        return True

    def _path(self):
        return self.path.split("?")[0]

    def do_OPTIONS(self):
        started = time.monotonic()
        origin = self._origin()
        if self._path() not in ("/chess-move", "/"):
            self._send(404, {"error": "not found"}, origin)
            log("OPTIONS", self._path(), 404, "none", started)
            return
        if origin not in ALLOWED_ORIGINS:
            # Fail closed: no preflight grant for an unknown origin.
            self._send(403, {"error": "origin not allowed"}, origin)
            log("OPTIONS", self._path(), 403, "none", started)
            return
        self.send_response(204)
        for key, value in cors_headers(origin).items():
            self.send_header(key, value)
        self.send_header("Content-Length", "0")
        self.end_headers()
        log("OPTIONS", self._path(), 204, "none", started)

    def do_GET(self):
        started = time.monotonic()
        origin = self._origin()
        if self._path() == "/health":
            # Opaque on purpose: model and engine names live in the journal.
            self._send(200, {"ok": True}, origin)
            log("GET", self._path(), 200, "none", started)
            return
        self._send(404, {"error": "not found"}, origin)
        log("GET", self._path(), 404, "none", started)

    def do_POST(self):
        started = time.monotonic()
        origin = self._origin()
        path = self._path()
        if path not in ("/chess-move", "/"):
            self._send(404, {"error": "not found"}, origin)
            log("POST", path, 404, "none", started)
            return
        if not LIMITER.allow():
            self._send(429, {"error": "too many requests"}, origin)
            log("POST", path, 429, "none", started)
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_BODY:
            self._send(400, {"error": "missing or oversized body"}, origin)
            log("POST", path, 400, "none", started)
            return
        if not SLOTS.acquire(timeout=SLOT_WAIT):
            self._send(503, {"error": "busy"}, origin, {"Retry-After": "1"})
            log("POST", path, 503, "none", started)
            return
        try:
            raw = self.rfile.read(length)
            fen, moves, board = parse_request(raw)
            move, source, usage = pick_move(fen, moves, board, started)
        except BadRequest as exc:
            self._send(400, {"error": str(exc)}, origin)
            log("POST", path, 400, "none", started)
            return
        except Exception as exc:
            # Stable, opaque message; the detail stays in the journal.
            self._send(500, {"error": "internal error"}, origin)
            log("POST", path, 500, "none", started, " detail=%s" % str(exc)[:200])
            return
        finally:
            SLOTS.release()
        self._send(200, {"move": move}, origin)
        log("POST", path, 200, source, started, " usage=%s" % json.dumps(usage) if usage else "")

    def log_message(self, fmt, *args):  # silence the default stderr access log
        return


ENGINE_NAME = None
ENGINE_OK = False


def main():
    global ENGINE_NAME, ENGINE_OK
    check_upstream_url(DEEPSEEK_URL)
    if not os.environ.get("DEEPSEEK_API_KEY"):
        sys.stderr.write("chess-bridge: warning: DEEPSEEK_API_KEY is not set; "
                         "every move will fall back to Stockfish\n")
    ENGINE_NAME = engine_version()
    if ENGINE_NAME is None:
        # Fail closed: without the engine neither the fallback nor the identity
        # check can be trusted, so refuse to serve rather than degrade silently.
        sys.stderr.write("chess-bridge: fatal: no working Stockfish at %s\n" % STOCKFISH)
        raise SystemExit(1)
    ENGINE_OK = ENGINE_NAME.startswith("Stockfish 17.1")
    if not ENGINE_OK:
        sys.stderr.write("chess-bridge: warning: Stockfish reports %r; "
                         "this build is pinned to Stockfish 17.1\n" % ENGINE_NAME)
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write("chess-bridge: listening on http://%s:%d (model=%s, engine=%s, "
                     "max_concurrency=%d, rate=%d/min burst=%d)\n"
                     % (BIND, PORT, MODEL, ENGINE_NAME, MAX_CONCURRENCY,
                        RATE_LIMIT_PER_MIN, RATE_BURST))
    sys.stderr.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
