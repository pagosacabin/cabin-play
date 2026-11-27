#!/usr/bin/env python3
"""chess-bridge — a chess-move endpoint for the nordtronics.io chess page.

POST /chess-move  {"fen": "...", "moves": ["e2e4", ...]}  ->  {"move": "<uci>", "comment": "<text>"}

v2 Architecture: Stockfish proposes candidate moves via MultiPV, and DeepSeek
(deepseek-flash) chooses one candidate and provides a friendly explanation.
If detectable, a blunder note about the player's previous move is passed to the coach.
If the model call fails or runs out of time, Stockfish's top candidate is returned
with comment "" so the game never stalls.

The DeepSeek key is read from the environment (DEEPSEEK_API_KEY) and is never
written to a file, logged, or echoed in a response.

Hardening:
  * upstream URL scheme allowlist — the bearer key never travels over cleartext
    unless the target is loopback;
  * a global token bucket and a bounded worker pool, so an unauthenticated
    caller cannot run up the API bill or exhaust threads;
  * one deadline for the whole handler (engine step, blunder pre-eval, model attempt,
    retry all draw from it), and the engine is limited by the remaining time;
  * CORS fails closed: an origin outside the allowlist gets no ACAO header and
    a 403 on preflight (no more substituting a trusted origin);
  * move history is UCI-syntax checked at parse time;
  * difficulty and personality parameters are strictly allowlisted;
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


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Refuse every redirect: a 30x can otherwise carry the bearer token to a
    cleartext or foreign host, defeating the startup scheme allowlist."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OPENER = urllib.request.build_opener(_NoRedirect)

BIND = "127.0.0.1"
PORT = int(os.environ.get("CHESS_BRIDGE_PORT", "8765"))

DEEPSEEK_URL = os.environ.get("CHESS_BRIDGE_URL", "https://api.deepseek.com/v1/chat/completions")
MODEL = os.environ.get("CHESS_BRIDGE_MODEL", "deepseek-flash")
STOCKFISH = os.environ.get("CHESS_BRIDGE_STOCKFISH", "/home/astroboy/.local/bin/stockfish")

MODEL_TIMEOUT = 2.6          # per HTTP attempt, seconds (was 4.0)
MODEL_MAX_TOKENS = 220
MODEL_TEMPERATURE = 0.3
RETRY_MIN_REMAINING = 0.8    # Keep the retry only when at least 0.8 s remains

STOCKFISH_TIMEOUT = 3.0
HANDLER_BUDGET = 5.5         # whole request, seconds (page aborts at 6 s)

MAX_BODY = 8192
MAX_FEN = 128
MAX_MOVES = 512
MAX_MOVE_LEN = 5

DIFFICULTY_CONFIG = {
    "easy": {"candidates": 3, "depth": 8, "time": 0.25},
    "normal": {"candidates": 4, "depth": 12, "time": 0.45},
    "hard": {"candidates": 5, "depth": 16, "time": 0.80},
}

# Concurrency and cost controls. The endpoint is unauthenticated by design
# (the page cannot log in), so the only bound on spend is here.
MAX_CONCURRENCY = int(os.environ.get("CHESS_BRIDGE_MAX_CONCURRENCY", "4"))
SLOTS = threading.BoundedSemaphore(MAX_CONCURRENCY)
SLOT_WAIT = 0.25             # seconds to wait for a free worker before 503
RATE_LIMIT_PER_MIN = int(os.environ.get("CHESS_BRIDGE_RATE_PER_MIN", "60"))
RATE_BURST = int(os.environ.get("CHESS_BRIDGE_RATE_BURST", "10"))

HEADER_TIMEOUT = 5.0         # socket read timeout: bounds a slow-drip client
MAX_HEADER_BYTES = 8192

# Engine admission control. A search that outlives the handler deadline is
# abandoned rather than waited for, but the python-chess thread cannot be
# killed — so engine work is serialised and the number of live searches is
# capped. Past the cap the bridge answers 500 and the page falls back, which is
# better than piling up engine processes behind a stalled one.
ENGINE_LOCK = threading.Lock()
MAX_ENGINE_INFLIGHT = 2
ENGINE_STATE = {"inflight": 0, "abandoned": 0}
ENGINE_STATE_LOCK = threading.Lock()

ALLOWED_ORIGINS = ("https://nordtronics.io", "https://www.nordtronics.io")

UCI_RE = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")

SYSTEM_PROMPT = (
    "You are a friendly chess coach playing against a person who is not a strong player. "
    "Pick one move from the candidates and explain it in one or two short sentences, "
    "using plain words, no jargon, no lectures. "
    "When the blunder note is present, say what was wrong with his move and what was better. "
    "Reply with JSON only."
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


def parse_difficulty(val):
    if not isinstance(val, str):
        return "normal"
    val = val.strip().lower()
    if val == "medium":
        return "normal"
    if val in ("easy", "normal", "hard"):
        return val
    return "normal"


def parse_personality(val):
    if not isinstance(val, str):
        return "default"
    val = val.strip().lower()
    if val in ("aggressive", "teach me", "default"):
        return val
    return "default"


def parse_model_reply(text, candidate_ucis, board):
    if not text:
        return None, "", "model reply was empty"
    cleaned = text.strip()
    fence_match = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", cleaned, re.DOTALL)
    if fence_match:
        raw_json = fence_match.group(1)
    else:
        obj_match = re.search(r"\{.*?\}", cleaned, re.DOTALL)
        if obj_match:
            raw_json = obj_match.group(0)
        else:
            return None, "", "no JSON object found in model reply (%r)" % text[:60]
    try:
        data = json.loads(raw_json)
    except Exception as exc:
        return None, "", "invalid JSON in model reply: %s" % exc
    if not isinstance(data, dict):
        return None, "", "model reply JSON is not an object"
    move = data.get("move")
    comment = data.get("comment", "")
    if not isinstance(move, str):
        return None, "", "model reply missing 'move' string"
    move = move.strip()
    if not isinstance(comment, str):
        comment = str(comment) if comment is not None else ""
    comment = comment.strip()
    if move not in candidate_ucis:
        return None, "", "model answered %s, which is not in the candidate list" % move
    try:
        chess_move = chess.Move.from_uci(move)
    except Exception:
        return None, "", "model move %s is not valid UCI" % move
    if chess_move not in board.legal_moves:
        return None, "", "model answered %s, which is not in the legal list" % move
    return move, comment, None


def ask_model(fen, moves, candidates, difficulty, personality, blunder_note, board, deadline, prior_error=None):
    """One DeepSeek completion. Returns (move_or_None, comment_or_empty, error_text, usage)."""
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        return None, "", "no DEEPSEEK_API_KEY in the service environment", None
    timeout = min(MODEL_TIMEOUT, deadline - time.monotonic())
    if timeout <= 0.2:
        return None, "", "no time left for a model call", None

    cand_strs = []
    cand_ucis = set()
    for uci, cp, mate in candidates:
        cand_ucis.add(uci)
        if mate is not None:
            m_str = f"+#{mate}" if mate > 0 else f"-#{abs(mate)}"
            cand_strs.append(f"{uci} {m_str}")
        else:
            cand_strs.append(f"{uci} {cp / 100.0:+.2f}")
    candidate_list_str = ", ".join(cand_strs)

    parts = [
        "FEN: %s" % fen,
        "Moves so far (UCI): %s" % (" ".join(moves) if moves else "(none)"),
        "Candidate moves: %s" % candidate_list_str,
        "Difficulty: %s" % difficulty,
        "Personality: %s" % personality,
    ]
    if blunder_note:
        parts.append(blunder_note)
    if prior_error:
        parts.append("Your previous answer was rejected: %s" % prior_error)
    parts.append('Reply with a JSON object: {"move": "<uci from the candidate list>", "comment": "<one or two short sentences, plain words>"}')

    body = {
        "model": MODEL,
        "thinking": {"type": "disabled"},
        "temperature": MODEL_TEMPERATURE,
        "max_tokens": MODEL_MAX_TOKENS,
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
        with OPENER.open(req, timeout=timeout) as resp:
            if resp.status != 200:
                return None, "", "model HTTP %s" % resp.status, None
            payload = json.loads(resp.read(65536).decode("utf-8", "replace"))
    except urllib.error.HTTPError as exc:
        return None, "", "model HTTP %s" % exc.code, None
    except Exception as exc:
        return None, "", "model call failed: %s" % type(exc).__name__, None

    usage = payload.get("usage") if isinstance(payload, dict) else None
    try:
        text = payload["choices"][0]["message"]["content"] or ""
    except Exception:
        return None, "", "model reply had no content", usage

    move, comment, err = parse_model_reply(text, cand_ucis, board)
    if err:
        return None, "", err, usage
    return move, comment, None, usage


def run_engine_step(board, fen, moves, difficulty, deadline):
    """Run Stockfish MultiPV for candidates and optional shallow pre-move blunder eval.
    Returns (candidates, max_depth, blunder_note, error_str).
    candidates is [(uci, score_cp, mate), ...] best first.
    """
    if not os.path.isfile(STOCKFISH) or not os.access(STOCKFISH, os.X_OK):
        return None, 0, None, "stockfish not executable at %s" % STOCKFISH
    remaining = deadline - time.monotonic()
    if remaining <= 0.2:
        return None, 0, None, "no time left for stockfish"
    budget = min(STOCKFISH_TIMEOUT, remaining)
    with ENGINE_STATE_LOCK:
        if ENGINE_STATE["inflight"] >= MAX_ENGINE_INFLIGHT:
            return None, 0, None, ("stockfish saturated: %d searches in flight, %d abandoned"
                                  % (ENGINE_STATE["inflight"], ENGINE_STATE["abandoned"]))
        ENGINE_STATE["inflight"] += 1
    if not ENGINE_LOCK.acquire(timeout=max(0.05, budget - 0.3)):
        with ENGINE_STATE_LOCK:
            ENGINE_STATE["inflight"] -= 1
        return None, 0, None, "stockfish busy"

    box = {}

    def _work():
        try:
            box["result"] = _engine_search_and_blunder(board, fen, moves, difficulty, budget, deadline)
        except Exception as exc:
            box["result"] = (None, 0, None, "stockfish failed: %s" % type(exc).__name__)
        finally:
            with ENGINE_STATE_LOCK:
                ENGINE_STATE["inflight"] -= 1
            ENGINE_LOCK.release()

    worker = threading.Thread(target=_work, name="stockfish", daemon=True)
    worker.start()
    worker.join(max(0.05, budget))
    if worker.is_alive():
        with ENGINE_STATE_LOCK:
            ENGINE_STATE["abandoned"] += 1
        return None, 0, None, "stockfish exceeded the handler budget"
    return box.get("result", (None, 0, None, "stockfish did not answer"))


def _engine_search_and_blunder(board, fen, moves, difficulty, budget, deadline):
    cfg = DIFFICULTY_CONFIG[difficulty]
    target_candidates = cfg["candidates"]
    depth_ceiling = cfg["depth"]
    time_cap = cfg["time"]

    legal = legal_moves(board)
    if not legal:
        return None, 0, None, "position has no legal moves"

    multipv_n = min(target_candidates, len(legal))

    try:
        with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as engine:
            name = (engine.id or {}).get("name") or ""
            if not name.startswith("Stockfish 17.1"):
                return None, 0, None, "stockfish identity changed: %r" % name[:40]
            engine.configure({"Threads": 1, "Hash": 16})

            # 1. Candidate search on current board
            cand_limit = chess.engine.Limit(depth=depth_ceiling, time=time_cap)
            infos = engine.analyse(board, cand_limit, multipv=multipv_n)
            if isinstance(infos, dict):
                infos = [infos]

            candidates = []
            max_depth = 0
            for info in infos:
                pv = info.get("pv")
                if not pv:
                    continue
                uci = pv[0].uci()
                if uci not in legal:
                    continue
                score_obj = info.get("score")
                if score_obj is None:
                    continue
                pov = score_obj.pov(board.turn)
                cp = pov.score(mate_score=10000)
                mate = pov.mate()
                depth = info.get("depth", 0)
                candidates.append((uci, cp, mate))
                if depth > max_depth:
                    max_depth = depth

            if not candidates:
                return None, 0, None, "stockfish produced no valid candidates"

            # Sort best first by score (mate_score=10000 ensures stable descending order)
            candidates.sort(key=lambda c: c[1], reverse=True)

            # 2. Shallow blunder detection if moves is non-empty
            blunder_note = None
            if moves and (deadline - time.monotonic() > 0.25):
                blunder_note = _detect_blunder(engine, board, fen, moves, candidates)

            return candidates, max_depth, blunder_note, None
    except Exception as exc:
        return None, 0, None, "stockfish failed: %s" % type(exc).__name__


def _detect_blunder(engine, board, fen, moves, candidates):
    try:
        pre_board = None
        # Try from starting board first (when full history from startpos is sent)
        try:
            b = chess.Board()
            for m in moves[:-1]:
                b.push_uci(m)
            b_after = b.copy()
            b_after.push_uci(moves[-1])
            if b_after.board_fen() == board.board_fen():
                pre_board = b
        except Exception:
            pre_board = None

        if pre_board is None:
            # Fallback: the FEN is the position BEFORE the last move and the
            # history is only a tail. Replaying moves[:-1] on the current FEN
            # would otherwise invent a position, so the candidate pre_board is
            # accepted only if pushing the last move lands on the given board.
            try:
                b = chess.Board(fen)
                for m in moves[:-1]:
                    b.push_uci(m)
                check = b.copy()
                check.push_uci(moves[-1])
                pre_board = b if check.board_fen() == board.board_fen() else None
            except Exception:
                pre_board = None

        if pre_board is None or not pre_board.is_valid():
            return None

        # Run shallow Stockfish analyse (multipv 1, Limit(depth=8, time=0.15))
        pre_limit = chess.engine.Limit(depth=8, time=0.15)
        pre_res = engine.analyse(pre_board, pre_limit)
        if isinstance(pre_res, list):
            pre_res = pre_res[0] if pre_res else {}

        pv = pre_res.get("pv")
        if not pv:
            return None
        preferred_move = pv[0].uci()

        score_obj = pre_res.get("score")
        if score_obj is None:
            return None

        pov_pre = score_obj.pov(pre_board.turn)
        # Guard: a mate score in pre-eval must silently degrade
        if pov_pre.mate() is not None:
            return None
        eval_before = pov_pre.score()
        if eval_before is None:
            return None

        # Player's evaluation after his own move is minus the current position's best score
        best_cand = candidates[0]
        eval_after = -best_cand[1]
        eval_after_mate = -best_cand[2] if best_cand[2] is not None else None

        if (eval_before - eval_after) >= 150:
            last_move = moves[-1]
            eval_before_str = f"{eval_before / 100.0:+.1f}"
            if eval_after_mate is not None:
                eval_after_str = f"+#{eval_after_mate}" if eval_after_mate > 0 else f"-#{abs(eval_after_mate)}"
            else:
                eval_after_str = f"{eval_after / 100.0:+.1f}"
            better_move_str = f"{preferred_move} ({eval_before_str})"

            return (
                f"BLUNDER NOTE: his previous move {last_move} was a mistake — "
                f"it dropped the evaluation from {eval_before_str} to {eval_after_str} for him; "
                f"a much better move was {better_move_str}. "
                f"Say plainly and briefly what was wrong with {last_move} and what was better."
            )
    except Exception:
        return None
    return None


def parse_request(raw):
    try:
        doc = json.loads(raw.decode("utf-8"))
    except Exception:
        raise BadRequest("body is not JSON")
    if not isinstance(doc, dict):
        raise BadRequest("body must be a JSON object")
    fen = doc.get("fen")
    moves = doc.get("moves", [])
    difficulty = parse_difficulty(doc.get("difficulty"))
    personality = parse_personality(doc.get("personality"))
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
        # Tolerantly clear invalid EP square if that is the only defect
        if board.status() == chess.STATUS_INVALID_EP_SQUARE:
            board.ep_square = None
        if not board.is_valid():
            raise BadRequest("fen is not a legal position")
        # Keep the prompt and the engine on one position: the model is shown the
        # normalised FEN (no phantom en-passant square), not the rejected input.
        fen = board.fen()
    return fen, clean, board, difficulty, personality


def pick_move(fen, moves, board, difficulty, personality, started):
    deadline = started + HANDLER_BUDGET
    legal = legal_moves(board)
    if not legal:
        raise BadRequest("the position has no legal moves")

    # 1. Engine step: candidate generation + blunder detection
    candidates, engine_depth, blunder_note, ferr = run_engine_step(
        board, fen, moves, difficulty, deadline
    )
    if not candidates:
        raise RuntimeError("engine: %s" % (ferr or "failed to generate candidates"))

    fallback_move = candidates[0][0]

    # Check if time remains for model call
    remaining = deadline - time.monotonic()
    if remaining <= 0.2:
        return fallback_move, "", "stockfish", None, "no time left for model call", len(candidates), engine_depth

    # 2. Model call
    move, comment, err, usage = ask_model(
        fen, moves, candidates, difficulty, personality, blunder_note, board, deadline
    )
    if move is None and (deadline - time.monotonic()) >= RETRY_MIN_REMAINING:
        move, comment, err2, usage2 = ask_model(
            fen, moves, candidates, difficulty, personality, blunder_note, board, deadline, prior_error=err
        )
        if move is None:
            err = err2 or err
        else:
            usage = usage2 or usage

    if move is not None:
        return move, comment, "model", usage, None, len(candidates), engine_depth

    # Fallback = Stockfish's best candidate (candidate[0]) with comment "" (empty string).
    return fallback_move, "", "stockfish", usage, err, len(candidates), engine_depth


class HeaderTooLarge(Exception):
    pass


class BoundedReader:
    """Counts bytes while the request line and headers are read.

    BaseHTTPRequestHandler reads headers one line at a time with no total cap,
    so a post-parse size check still lets a client make the process buffer an
    arbitrary header block. This wrapper stops the read itself.
    """

    def __init__(self, stream, limit):
        self.stream = stream
        self.left = limit
        self.enforcing = True

    def readline(self, size=-1):
        line = self.stream.readline(size)
        if self.enforcing:
            self.left -= len(line)
            if self.left < 0:
                raise HeaderTooLarge()
        return line

    def read(self, n=-1):
        return self.stream.read(n)

    def __getattr__(self, name):
        return getattr(self.stream, name)


class Handler(BaseHTTPRequestHandler):
    server_version = "chess-bridge/2.0"
    protocol_version = "HTTP/1.1"
    timeout = HEADER_TIMEOUT  # a slow-drip client cannot hold a worker forever

    def setup(self):
        BaseHTTPRequestHandler.setup(self)
        self.rfile = BoundedReader(self.rfile, MAX_HEADER_BYTES)

    def handle_one_request(self):
        try:
            BaseHTTPRequestHandler.handle_one_request(self)
        except HeaderTooLarge:
            started = time.monotonic()
            self.close_connection = True
            # The request line may be half-read, so fill in what send_response
            # and our own log line expect before writing the answer.
            self.requestline = getattr(self, "requestline", "")
            self.request_version = "HTTP/1.1"
            if getattr(self, "headers", None) is None:
                self.headers = self.MessageClass()
            self.command = getattr(self, "command", None) or "?"
            self.path = getattr(self, "path", None) or "?"
            try:
                self._send(431, {"error": "request headers too large"}, self._origin(), drain=True)
            except Exception as exc:
                sys.stderr.write("chess-bridge: 431 write failed: %r\n" % exc)
            log(self.command, self.path, 431, "none", started)

    def send_error(self, code, message=None, explain=None):
        # Anything the routing methods do not handle (an unknown method, a
        # malformed request line) still leaves one journal line.
        try:
            log(getattr(self, "command", "?") or "?", getattr(self, "path", "?") or "?",
                code, "none", getattr(self, "_started", time.monotonic()))
        finally:
            BaseHTTPRequestHandler.send_error(self, code, message, explain)

    def _origin(self):
        headers = getattr(self, "headers", None)
        return (headers.get("Origin", "") if headers is not None else "")

    def _drain(self):
        """Discard unread request bytes before closing.

        A close with data still queued in the socket send buffer makes the
        kernel answer RST, which throws away the response we just wrote — the
        client sees EOF instead of the 4xx.
        """
        try:
            self.connection.settimeout(0.2)
            for _ in range(16):
                if not self.connection.recv(8192):
                    break
        except Exception:
            pass

    def _send(self, status, payload, origin, extra=None, drain=False):
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
        if drain:
            self._drain()

    def parse_request(self):
        if not BaseHTTPRequestHandler.parse_request(self):
            return False
        if hasattr(self.rfile, "enforcing"):
            self.rfile.enforcing = False  # the body read is bounded separately
        size = sum(len(k) + len(v) for k, v in self.headers.items())
        if size > MAX_HEADER_BYTES:  # belt and braces under the read cap
            self._send(431, {"error": "request headers too large"}, self._origin())
            log(self.command or "?", self.path or "?", 431, "none", time.monotonic())
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
            self._send(400, {"error": "missing or oversized body"}, origin, drain=True)
            log("POST", path, 400, "none", started)
            return
        if not SLOTS.acquire(timeout=SLOT_WAIT):
            self._send(503, {"error": "busy"}, origin, {"Retry-After": "1"}, drain=True)
            log("POST", path, 503, "none", started)
            return
        try:
            raw = self.rfile.read(length)
            fen, moves, board, difficulty, personality = parse_request(raw)
            move, comment, source, usage, note, cand_count, engine_depth = pick_move(
                fen, moves, board, difficulty, personality, started
            )
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
        self._send(200, {"move": move, "comment": comment}, origin)
        extra = " difficulty=%s candidates=%d depth=%d comment=%s" % (
            difficulty, cand_count, engine_depth, "model" if comment else "empty"
        )
        if usage:
            extra += " usage=%s" % json.dumps(usage)
        if source != "model" and note:
            extra += " detail=%s" % str(note)[:200]
        log("POST", path, 200, source, started, extra)

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
        # The pin is a claim the service makes in its README and health output;
        # serving on a different engine would make it quietly false.
        sys.stderr.write("chess-bridge: fatal: Stockfish reports %r; "
                         "this build is pinned to Stockfish 17.1\n" % ENGINE_NAME)
        raise SystemExit(1)
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
