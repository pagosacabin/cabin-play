#!/usr/bin/env python3
"""pocket-conquest-bridge — an agent endpoint for the Pocket Conquest page.

POST /pocket-conquest-move  {"position": {...}, "moves": ["...", ...]}
     ->  {"move": "M B5 B4 3"}          a legal move from the page's list
     ->  HTTP 400/429/500 {"error": ...}  anything else, so the page falls back
                                          to its built-in computer

The opponent is DeepSeek (deepseek-flash by default), the same key and model
Hermes uses for his own work. Because the model has never seen this game, every
prompt carries the full rules, the board, gold and armies, and the numbered list
of legal moves. The answer is validated against the rules engine before it is
returned; an answer that is not in the list is retried and then refused.

The DeepSeek key is read from the environment (DEEPSEEK_API_KEY) and is never
written to a file, logged, or echoed in a response.

Hardening (inherited from the reviewed chess bridge):
  * upstream URL scheme allowlist — the bearer key never travels over cleartext
    unless the target is loopback;
  * a global token bucket and a bounded worker pool, so an unauthenticated
    caller cannot run up the API bill or exhaust threads;
  * one deadline for the whole handler, and every model attempt draws from it;
  * a bounded header block and body, a socket read timeout, and one journal line
    per request;
  * opaque 500s (the detail stays in the journal) and a fail-closed answer: if
    the model cannot produce a legal move the endpoint returns 500, which the
    page already treats as "use the built-in computer".
"""

import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import agent_core
import game_state as gs
import pocket_conquest as pc


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Refuse every redirect: a 30x can otherwise carry the bearer token to a
    cleartext or foreign host, defeating the startup scheme allowlist."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OPENER = urllib.request.build_opener(_NoRedirect)

BIND = "127.0.0.1"
PORT = int(os.environ.get("PC_BRIDGE_PORT", "8767"))

DEEPSEEK_URL = os.environ.get("PC_BRIDGE_URL", "https://api.deepseek.com/v1/chat/completions")
MODEL = os.environ.get("PC_BRIDGE_MODEL", "deepseek-flash")

MODEL_TIMEOUT = 3.2          # per HTTP attempt, seconds
MODEL_MAX_TOKENS = 32        # a move string is a handful of tokens
MODEL_TEMPERATURE = 0.0
RETRY_MIN_REMAINING = 1.0    # keep the retry only when this much time is left

HANDLER_BUDGET = 5.5         # whole request; the page's own client timeout is 6 s

MAX_BODY = 16384
MAX_MOVES = 400
MAX_MOVE_LEN = 48

MAX_CONCURRENCY = int(os.environ.get("PC_BRIDGE_MAX_CONCURRENCY", "4"))
SLOTS = threading.BoundedSemaphore(MAX_CONCURRENCY)
SLOT_WAIT = 0.25

RATE_LIMIT_PER_MIN = int(os.environ.get("PC_BRIDGE_RATE_PER_MIN", "60"))
RATE_BURST = int(os.environ.get("PC_BRIDGE_RATE_BURST", "10"))

HEADER_TIMEOUT = 5.0
MAX_HEADER_BYTES = 8192

# The page's own origin is not fixed (it is an artifact page), so an allowlist is
# optional: with no PC_BRIDGE_ALLOWED_ORIGINS set, any origin is granted a CORS
# read (the endpoint carries no credentials and no cookies, and the token bucket
# is the spend bound). Set the variable to pin it to known origins.
ALLOWED_ORIGINS = tuple(
    o.strip() for o in os.environ.get("PC_BRIDGE_ALLOWED_ORIGINS", "").split(",") if o.strip()
)
# Tailscale Serve with --set-path keeps the prefix in the forwarded path (proven on
# the chess bridge, /chese-move), so under the mount `/pocket-conquest` the page's
# POST arrives as either `/pocket-conquest` (mount root), `/pocket-conquest/` or
# `/pocket-conquest/pocket-conquest-move`. All three are accepted, plus the bare
# path when the service is called directly over localhost.
PATHS = (
    "/pocket-conquest-move",
    "/pocket-conquest",
    "/pocket-conquest/",
    "/pocket-conquest/pocket-conquest-move",
    "/",
)
HEALTH_PATHS = ("/health", "/pocket-conquest/health")
# The page's Juno mode (index.html) calls three more routes on this same base
# URL: POST /games, POST /games/{id}/move, GET /games/{id}. Under the Serve
# mount they arrive prefixed, so the mount prefix is stripped before routing.
MOUNT_PREFIX = "/pocket-conquest"
GAME_MOVE_BUDGET = 20.0      # the page's own client allows 30 s for this call
MAX_LEGAL_FOR_PROMPT = 240   # bound the prompt (and so the model spend) by input size
GAMES = gs.GameStore()


def _ply(position):
    """The page's ply for an engine position (nord's turn is odd)."""
    return 2 * position["turn"] - (1 if position["side"] == "S" else 0)


def bounded_legal(legal):
    """Cap the legal-move list that reaches the model.

    The caller supplies the position, so the list length is attacker-influenced:
    a maximal board yields thousands of `M ... n` strings. Every entry is a legal
    move, so a deterministic prefix-free subset keeps the opponent sane while
    bounding prompt tokens and model time. Ordering is preserved (the engine
    lists builds, then moves, then END), and END is always kept last.
    """
    if len(legal) <= MAX_LEGAL_FOR_PROMPT:
        return legal
    if "END" in legal:
        head = [m for m in legal if m != "END"][:MAX_LEGAL_FOR_PROMPT - 1]
        return head + ["END"]
    return legal[:MAX_LEGAL_FOR_PROMPT]


def _strip_mount(path):
    """Drop the Serve mount prefix; return the service-relative path."""
    if path == MOUNT_PREFIX:
        return "/"
    if path.startswith(MOUNT_PREFIX + "/"):
        return path[len(MOUNT_PREFIX):]
    return path


def route(path):
    """Map a request path to (kind, game_id) or (None, None).

    kind is one of: move (the legacy single-move endpoint), health, games
    (collection), game (one game), game_move (submit a turn).
    """
    rel = _strip_mount(path)
    if rel in ("/", "/pocket-conquest-move"):
        return "move", None
    if rel == "/health":
        return "health", None
    if rel == "/games":
        return "games", None
    if rel.startswith("/games/"):
        rest = rel[len("/games/"):]
        if rest.endswith("/move"):
            gid = rest[:-len("/move")]
            return ("game_move", gid) if gs.valid_game_id(gid) else (None, None)
        return ("game", rest) if gs.valid_game_id(rest) else (None, None)
    return None, None

SYSTEM_PROMPT = (
    "You are a cautious commander in a small turn-based strategy game. "
    "Choose exactly one move from the numbered list of legal moves and reply with "
    "that move and nothing else. Any other text is discarded."
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
    raise SystemExit("pocket-conquest-bridge: refusing PC_BRIDGE_URL=%r: it must be https, "
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
# Game creation gets its own, tighter bucket: a created game occupies one of the
# MAX_GAMES slots, so without this a tailnet peer could churn the table and evict
# live games (review pass 2, finding 1). Moves and polls share the global bucket.
CREATE_PER_MIN = int(os.environ.get("PC_BRIDGE_CREATE_PER_MIN", "20"))
CREATE_BURST = int(os.environ.get("PC_BRIDGE_CREATE_BURST", "5"))
CREATE_LIMITER = RateLimiter(CREATE_PER_MIN, CREATE_BURST)


class BadRequest(Exception):
    pass


def log(method, path, status, source, started, extra=""):
    ms = int((time.monotonic() - started) * 1000)
    sys.stderr.write("pocket-conquest-move method=%s path=%s status=%s source=%s elapsed_ms=%d%s\n"
                     % (method, path, status, source, ms, extra))
    sys.stderr.flush()


def cors_headers(origin):
    """ACAO for the allowlisted origins, or for any origin when none is pinned.

    Echoing the caller's Origin is safe only because this endpoint carries no
    cookies, no credentials and no session state. If either is ever added, pin
    PC_BRIDGE_ALLOWED_ORIGINS instead of echoing.
    """
    if not ALLOWED_ORIGINS:
        return {
            "Access-Control-Allow-Origin": origin or "*",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "content-type",
            "Access-Control-Max-Age": "600",
            "Vary": "Origin",
        }
    if origin not in ALLOWED_ORIGINS:
        return {"Vary": "Origin"}
    return {
        "Access-Control-Allow-Origin": origin,
        "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
        "Access-Control-Allow-Headers": "content-type",
        "Access-Control-Max-Age": "600",
        "Vary": "Origin",
    }


def origin_allowed(origin):
    return not ALLOWED_ORIGINS or origin in ALLOWED_ORIGINS


def ask_model(prompt, deadline, prior_error=None):
    """One DeepSeek attempt. Returns (text, error, usage)."""
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        return None, "no DEEPSEEK_API_KEY in the service environment", None
    remaining = deadline - time.monotonic()
    if remaining <= 0.3:
        return None, "no time left for the model call", None
    user = prompt
    if prior_error:
        user += "\n\nYour previous answer was rejected: %s. Answer with exactly one move from the list." % prior_error
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user},
        ],
        "temperature": MODEL_TEMPERATURE,
        "max_tokens": MODEL_MAX_TOKENS,
        # deepseek-flash is a reasoning model: without this the whole token
        # budget is spent on hidden reasoning and `content` comes back empty.
        # The task forbids long reasoning anyway, and a move needs no chain.
        "thinking": {"type": "disabled"},
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(DEEPSEEK_URL, data=data, method="POST")
    request.add_header("Content-Type", "application/json")
    request.add_header("Authorization", "Bearer " + key)
    timeout = max(0.3, min(MODEL_TIMEOUT, remaining - 0.1))
    try:
        with OPENER.open(request, timeout=timeout) as response:
            raw = response.read(65536)
        doc = json.loads(raw.decode("utf-8"))
        text = doc["choices"][0]["message"]["content"]
        usage = doc.get("usage") or None
        if not isinstance(text, str):
            return None, "model reply had no text", usage
        return text, None, usage
    except urllib.error.HTTPError as exc:
        return None, "upstream HTTP %s" % exc.code, None
    except Exception as exc:
        return None, "upstream error: %s" % type(exc).__name__, None


def parse_request(raw):
    """Validate the wire document and return (position, page_moves, legal)."""
    try:
        doc = json.loads(raw.decode("utf-8"))
    except Exception:
        raise BadRequest("body is not JSON")
    if not isinstance(doc, dict):
        raise BadRequest("body must be a JSON object")
    if "position" not in doc:
        raise BadRequest("body must carry a position object")
    try:
        position = pc.normalise_position(doc["position"])
    except ValueError as exc:
        raise BadRequest("position: %s" % exc)
    except Exception:
        raise BadRequest("position is not a valid position")
    page_moves = doc.get("moves", [])
    if page_moves is None:
        page_moves = []
    if not isinstance(page_moves, list) or len(page_moves) > MAX_MOVES:
        raise BadRequest("moves must be a list of at most %d entries" % MAX_MOVES)
    clean = []
    for item in page_moves:
        if not isinstance(item, str) or not item or len(item) > MAX_MOVE_LEN:
            raise BadRequest("every move must be a short string")
        clean.append(item.strip())
    legal = pc.legal_moves(position)
    if not legal:
        raise BadRequest("the position has no legal moves")
    chosen_set = [m for m in clean if m in set(legal)] if clean else list(legal)
    if not chosen_set:
        # The page's list and the engine disagree: trust the engine (it is the
        # rules authority) but say so in the journal rather than guessing.
        sys.stderr.write("pocket-conquest-bridge: page listed %d moves, none legal; "
                         "using the engine's %d\n" % (len(clean), len(legal)))
        chosen_set = list(legal)
    return position, clean, chosen_set


def pick_move(position, legal, started):
    """Ask the model for one legal move. Returns (move, usage, error, attempts)."""
    deadline = started + HANDLER_BUDGET
    prompt = agent_core.build_prompt(position, legal)
    attempts = []
    usage = None
    prior_error = None
    for attempt in range(2):
        text, err, usage_now = ask_model(prompt, deadline, prior_error)
        usage = usage_now or usage
        if text is None:
            attempts.append({"attempt": attempt + 1, "error": err})
            prior_error = err
        else:
            move = agent_core.parse_reply(text, legal)
            if move is not None:
                attempts.append({"attempt": attempt + 1, "ok": True})
                return move, usage, None, attempts
            attempts.append({"attempt": attempt + 1, "error": "reply was not a legal move"})
            prior_error = "not one of the listed moves"
        if deadline - time.monotonic() < RETRY_MIN_REMAINING:
            break
    return None, usage, prior_error or "no legal move from the model", attempts


class HeaderTooLarge(Exception):
    pass


class BoundedReader:
    """Counts bytes while the request line and headers are read."""

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
        # Deliberately unmetered: only the *header* block goes through
        # readline(), and the body is read later under an explicit
        # Content-Length cap. Anything that starts calling read() during header
        # parsing would bypass the header cap, so keep this assumption in mind.
        return self.stream.read(n)

    def __getattr__(self, name):
        return getattr(self.stream, name)


class Handler(BaseHTTPRequestHandler):
    server_version = "pocket-conquest-bridge/1.0"
    protocol_version = "HTTP/1.1"
    timeout = HEADER_TIMEOUT

    def setup(self):
        BaseHTTPRequestHandler.setup(self)
        self.rfile = BoundedReader(self.rfile, MAX_HEADER_BYTES)

    def handle_one_request(self):
        try:
            BaseHTTPRequestHandler.handle_one_request(self)
        except HeaderTooLarge:
            started = time.monotonic()
            self.close_connection = True
            self.requestline = getattr(self, "requestline", "")
            self.request_version = "HTTP/1.1"
            if getattr(self, "headers", None) is None:
                self.headers = self.MessageClass()
            self.command = getattr(self, "command", None) or "?"
            self.path = getattr(self, "path", None) or "?"
            try:
                self._send(431, {"error": "request headers too large"}, self._origin(), drain=True)
            except Exception as exc:
                sys.stderr.write("pocket-conquest-bridge: 431 write failed: %r\n" % exc)
            log(self.command, self.path, 431, "none", started)

    def send_error(self, code, message=None, explain=None):
        try:
            log(getattr(self, "command", "?") or "?", getattr(self, "path", "?") or "?",
                code, "none", getattr(self, "_started", time.monotonic()))
        finally:
            BaseHTTPRequestHandler.send_error(self, code, message, explain)

    def _origin(self):
        headers = getattr(self, "headers", None)
        return (headers.get("Origin", "") if headers is not None else "")

    def _drain(self):
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
            self.rfile.enforcing = False
        size = sum(len(k) + len(v) for k, v in self.headers.items())
        if size > MAX_HEADER_BYTES:
            self.close_connection = True
            self._send(431, {"error": "request headers too large"}, self._origin(), drain=True)
            log(self.command or "?", self.path or "?", 431, "none", time.monotonic())
            return False
        return True

    def _path(self):
        return self.path.split("?")[0]

    def do_OPTIONS(self):
        started = time.monotonic()
        origin = self._origin()
        if route(self._path())[0] is None:
            self.close_connection = True
            self._send(404, {"error": "not found"}, origin, drain=True)
            log("OPTIONS", self._path(), 404, "none", started)
            return
        if not origin_allowed(origin):
            self.close_connection = True
            self._send(403, {"error": "origin not allowed"}, origin, drain=True)
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
        path = self._path()
        kind, gid = route(path)
        if kind == "health":
            self._send(200, {"ok": True}, origin)
            log("GET", path, 200, "none", started)
            return
        if kind == "game":
            if not origin_allowed(origin):
                self.close_connection = True
                self._send(403, {"error": "origin not allowed"}, origin, drain=True)
                log("GET", path, 403, "none", started)
                return
            position = GAMES.get(gid)
            if position is None:
                self.close_connection = True
                self._send(404, {"error": "no such game"}, origin, drain=True)
                log("GET", path, 404, "none", started)
                return
            self._send(200, {"id": gid, "game": "pocket-conquest",
                             "state": gs.page_state_from_engine(position)}, origin)
            log("GET", path, 200, "engine", started, " event=poll game=%s" % gid)
            return
        self._send(404, {"error": "not found"}, origin)
        log("GET", path, 404, "none", started)

    def do_POST(self):
        started = time.monotonic()
        origin = self._origin()
        path = self._path()
        kind, gid = route(path)
        if kind not in ("move", "games", "game_move"):
            # The body has not been read: close, or HTTP/1.1 keep-alive would
            # parse it back as the next request line on this socket.
            self.close_connection = True
            self._send(404, {"error": "not found"}, origin, drain=True)
            log("POST", path, 404, "none", started)
            return
        if not origin_allowed(origin):
            self.close_connection = True
            self._send(403, {"error": "origin not allowed"}, origin, drain=True)
            log("POST", path, 403, "none", started)
            return
        if not LIMITER.allow():
            self.close_connection = True
            self._send(429, {"error": "too many requests"}, origin, drain=True)
            log("POST", path, 429, "none", started)
            return
        if kind == "games" and not CREATE_LIMITER.allow():
            self.close_connection = True
            self._send(429, {"error": "too many games"}, origin, drain=True)
            log("POST", path, 429, "none", started)
            return
        # http.server does not decode chunked bodies, so a chunked request would
        # be read as raw framing bytes and could also slip past the size cap.
        if self.headers.get("Transfer-Encoding"):
            self.close_connection = True
            self._send(400, {"error": "chunked bodies are not accepted"}, origin, drain=True)
            log("POST", path, 400, "none", started)
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length <= 0 or length > MAX_BODY:
            self.close_connection = True
            self._send(400, {"error": "missing or oversized body"}, origin, drain=True)
            log("POST", path, 400, "none", started)
            return
        if not SLOTS.acquire(timeout=SLOT_WAIT):
            self.close_connection = True
            self._send(503, {"error": "busy"}, origin, {"Retry-After": "1"}, drain=True)
            log("POST", path, 503, "none", started)
            return
        try:
            if kind == "move":
                raw = self.rfile.read(length)
                position, page_moves, legal = parse_request(raw)
                move, usage, note, attempts = pick_move(position, legal, started)
            elif kind == "games":
                body = self._json_body(length)
                position = self._game_position(body, "S")
                new_id = GAMES.create(position)
                self._send(200, {"id": new_id, "game": "pocket-conquest",
                                 "state": gs.page_state_from_engine(position)}, origin)
                log("POST", path, 200, "engine", started, " event=create game=%s" % new_id)
                return
            else:
                body = self._json_body(length)
                position = self._game_position(body, "R")
                stored = GAMES.get(gid)
                if stored is not None and _ply(position) < _ply(stored):
                    # The page is authoritative for the board, but a state that
                    # rewinds the stored game is refused (review pass 3, LOW):
                    # the id alone must not let a caller replay an earlier ply.
                    raise BadRequest("state is older than the stored game")
                adopted = GAMES.adopt(gid, position)
                legal = pc.legal_moves(position)
                if not legal:
                    raise BadRequest("the position has no legal moves")
                move, usage, note, attempts = self._juno_turn(
                    position, bounded_legal(legal), started)
                if move is None:
                    self.close_connection = True
                    self._send(500, {"error": "no legal move from the opponent"},
                               origin, drain=True)
                    log("POST", path, 500, "model", started,
                        " event=game_move game=%s detail=%s attempts=%s"
                        % (gid, str(note)[:160], json.dumps(attempts)))
                    return
                updated = pc.apply_move(position, move)
                updated = pc.apply_move(updated, "END")
                GAMES.put(gid, updated)
                self._send(200, {"id": gid, "move": move,
                                 "state": gs.page_state_from_engine(updated)}, origin)
                extra = " event=game_move game=%s adopted=%s move=%s" % (
                    gid, adopted, move.replace(" ", "_"))
                if usage:
                    extra += " usage=%s" % json.dumps(usage)
                log("POST", path, 200, "model", started, extra)
                return
        except BadRequest as exc:
            # Close and drain: with HTTP/1.1 keep-alive a pipelined request left
            # in the socket would otherwise be parsed as this one's continuation.
            self.close_connection = True
            self._send(400, {"error": str(exc)}, origin, drain=True)
            log("POST", path, 400, "none", started)
            return
        except Exception as exc:
            self.close_connection = True
            self._send(500, {"error": "internal error"}, origin, drain=True)
            log("POST", path, 500, "none", started, " detail=%s" % str(exc)[:200])
            return
        finally:
            SLOTS.release()
        if move is None:
            # Never answer an illegal or random move: let the page use its own
            # computer instead. Close, so a retrying client starts a fresh
            # connection instead of burning another slot on a dead one.
            self.close_connection = True
            self._send(500, {"error": "no legal move from the opponent"}, origin)
            log("POST", path, 500, "model", started,
                " detail=%s attempts=%s" % (str(note)[:160], json.dumps(attempts)))
            return
        self._send(200, {"move": move}, origin)
        extra = " moves_listed=%d legal=%d attempts=%d" % (len(page_moves), len(legal), len(attempts))
        if usage:
            extra += " usage=%s" % json.dumps(usage)
        log("POST", path, 200, "model", started, extra)

    def _json_body(self, length):
        raw = self.rfile.read(length)
        try:
            doc = json.loads(raw.decode("utf-8"))
        except Exception:
            raise BadRequest("body is not JSON")
        if not isinstance(doc, dict):
            raise BadRequest("body must be a JSON object")
        if doc.get("game") not in (None, "pocket-conquest"):
            raise BadRequest("only the pocket-conquest game is supported")
        return doc

    def _game_position(self, doc, side):
        """Validate the page's state and convert it into an engine position."""
        if "state" not in doc:
            raise BadRequest("body must carry a state object")
        try:
            return gs.engine_position_from_page(doc["state"], side)
        except gs.GameStateError as exc:
            raise BadRequest(str(exc)[:200])

    def _juno_turn(self, position, legal, started):
        """Let DeepSeek pick one legal move, and report its usage."""
        deadline = started + GAME_MOVE_BUDGET
        usage = {}

        def model_call(prompt):
            text, err, used = ask_model(prompt, deadline)
            if used:
                usage.update(used)
            if text is None:
                raise RuntimeError(err or "the model call failed")
            return text

        move, trace = agent_core.pick_legal(position, model_call, retries=1)
        return move, usage or None, trace, trace

    def log_message(self, fmt, *args):
        return


def main():
    check_upstream_url(DEEPSEEK_URL)
    if not os.environ.get("DEEPSEEK_API_KEY"):
        # Fail closed: every request would end in a 500, which is worse than a
        # service that refuses to start and says why.
        sys.stderr.write("pocket-conquest-bridge: fatal: DEEPSEEK_API_KEY is not set\n")
        raise SystemExit(1)
    # Prove the engine imports and the initial position is self-consistent before
    # taking traffic.
    pc.legal_moves(pc.initial_position())
    server = ThreadingHTTPServer((BIND, PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write("pocket-conquest-bridge: listening on http://%s:%d (model=%s, "
                     "max_concurrency=%d, rate=%d/min burst=%d, origins=%s)\n"
                     % (BIND, PORT, MODEL, MAX_CONCURRENCY, RATE_LIMIT_PER_MIN,
                        RATE_BURST, ",".join(ALLOWED_ORIGINS) or "any"))
    sys.stderr.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
