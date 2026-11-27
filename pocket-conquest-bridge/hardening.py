#!/usr/bin/env python3
"""Raw-socket hardening checks for the Pocket Conquest bridge.

Each check drives the service the way a hostile caller would and asserts the
answer. Run against a scratch instance (a low rate limit makes the 429 check
quick):

    PC_BRIDGE_RATE_PER_MIN=6 PC_BRIDGE_RATE_BURST=3 PC_BRIDGE_PORT=8769 \
        python3 server.py &
    python3 hardening.py 8769

Prints one PASS/FAIL line per check and exits 1 if any check failed.
"""

import os
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8769
HOST = "127.0.0.1"


def raw(payload, read_until_close=False):
    """Send raw bytes, return everything the server answers."""
    sock = socket.create_connection((HOST, PORT), timeout=5)
    try:
        sock.sendall(payload)
        chunks = []
        sock.settimeout(2.0)
        while True:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
            if not read_until_close:
                break
        return b"".join(chunks)
    finally:
        sock.close()


def status_of(response):
    first = response.split(b"\r\n", 1)[0]
    parts = first.split(b" ")
    return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else -1


def check(name, response, expect):
    code = status_of(response)
    ok = code == expect
    print("%-52s -> HTTP %-4s %s" % (name, code, "PASS" if ok else "FAIL (wanted %d)" % expect))
    return ok


def post(path, body, headers=b""):
    return (b"POST " + path + b" HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            b"Content-Type: application/json\r\n" + headers +
            b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)


def main():
    failures = 0
    good = b'{"position": {"side": "S", "gold": 10, "grid": [["."],["."]]}}'

    failures += not check("garbage JSON body", raw(post(b"/pocket-conquest-move", b"{not json")), 400)
    failures += not check("no position key", raw(post(b"/pocket-conquest-move", b'{"moves": []}')), 400)
    failures += not check("unknown path", raw(post(b"/nope", good)), 404)
    failures += not check("unknown method (DELETE)", raw(b"DELETE /health HTTP/1.1\r\nHost: x\r\n\r\n"), 501)

    # An oversized Content-Length is refused before any body is read.
    oversized = (b"POST /pocket-conquest-move HTTP/1.1\r\nHost: x\r\n"
                 b"Content-Length: 999999\r\n\r\n")
    failures += not check("oversized Content-Length", raw(oversized), 400)

    # A chunked body must be refused outright, not read as framing bytes.
    chunked = (b"POST /pocket-conquest-move HTTP/1.1\r\nHost: x\r\n"
               b"Transfer-Encoding: chunked\r\n\r\n5\r\n{\"a\":\r\n0\r\n\r\n")
    failures += not check("Transfer-Encoding: chunked", raw(chunked), 400)

    # 16 KiB of headers must be cut off while they are read (431, not 400/200).
    bomb = (b"POST /pocket-conquest-move HTTP/1.1\r\nHost: x\r\n" +
            b"".join(b"X-Filler-%03d: %s\r\n" % (i, b"A" * 200) for i in range(90)) +
            b"\r\n")
    failures += not check("16 KiB header block", raw(bomb), 431)

    # After a rejected request the connection is closed, so a pipelined
    # follow-up must not be answered on the same socket.
    pipelined = raw(b"POST /pocket-conquest-move HTTP/1.1\r\nHost: x\r\n"
                    b"Content-Length: 3\r\n\r\nxxx" + post(b"/health", b"{}"), read_until_close=True)
    answers = pipelined.count(b"HTTP/1.1 ")
    ok = answers == 1
    print("%-52s -> %d answer(s) %s" % ("pipelined request after a rejected one",
                                       answers, "PASS" if ok else "FAIL"))
    failures += not ok

    # The same smuggling shape through the 404 path: the body is never read, so
    # the connection must close rather than parse it back as a request.
    smuggled = raw(b"POST /nope HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n"
                   % len(post(b"/health", b"{}")) + post(b"/health", b"{}"),
                   read_until_close=True)
    answers = smuggled.count(b"HTTP/1.1 ")
    ok = answers == 1
    print("%-52s -> %d answer(s) %s" % ("pipelined request behind a 404", answers,
                                        "PASS" if ok else "FAIL"))
    failures += not ok

    # GET /health and an OPTIONS preflight are the two legitimate shapes.
    failures += not check("GET /health", raw(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"), 200)
    preflight = (b"OPTIONS /pocket-conquest-move HTTP/1.1\r\nHost: x\r\n"
                 b"Origin: https://example.test\r\n"
                 b"Access-Control-Request-Method: POST\r\n\r\n")
    response = raw(preflight)
    allow = b"access-control-allow-origin" in response.lower()
    print("%-52s -> HTTP %-4s %s" % ("OPTIONS preflight", status_of(response),
                                     "PASS" if allow and status_of(response) == 204 else "FAIL"))
    failures += not (allow and status_of(response) == 204)

    # Burst past the limiter and require a 429 rather than a 500/200. This burns
    # the bucket, so it runs last and only under a low-limit instance.
    if os.environ.get("PC_CHECK_RATE") == "1":
        codes = [status_of(raw(post(b"/pocket-conquest-move", b"{}"))) for _ in range(12)]
        limited = 429 in codes
        print("%-52s -> %s %s" % ("rate limit reached under a burst", codes,
                                  "PASS" if limited else "FAIL"))
        failures += not limited
    else:
        print("%-52s -> skipped (set PC_CHECK_RATE=1 on a low-limit instance)"
              % "rate limit reached under a burst")

    print("\n%d failing check(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
