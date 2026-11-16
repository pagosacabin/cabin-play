#!/usr/bin/env python3
"""Live end-to-end smoke test for writing-peer-review (task 0186, criteria 1-6 and 8).

Runs the real server as a subprocess on a throwaway SQLite DB, exercises the HTTP API
over the network, restarts the server to prove SQLite persistence, and makes ONE real
DeepSeek review call (no stub).  PDF text is read back with `pdftotext`.

    ./.venv/bin/python tests/live_smoke.py

Exit code 0 = every check passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = int(os.environ.get("SMOKE_PORT", "8788"))
BASE = f"http://127.0.0.1:{PORT}"
TMP = Path("/tmp/wpr-smoke") if Path("/tmp").is_dir() else ROOT / ".smoke-tmp"
DB = TMP / "smoke.db"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return ok


def req(method, path, payload=None, raw=False):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    r = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(r, timeout=180) as resp:
        body = resp.read()
        return resp.status, dict(resp.headers), (body if raw else body.decode("utf-8"))


def start_server():
    env = dict(os.environ)
    env["WPR_DB"] = str(DB)
    p = subprocess.Popen(
        [str(ROOT / ".venv/bin/python"), "-m", "uvicorn", "app.main:app",
         "--host", "127.0.0.1", "--port", str(PORT), "--log-level", "warning"],
        cwd=str(ROOT), env=env,
    )
    for _ in range(120):
        try:
            req("GET", "/api/status")
            return p
        except Exception:
            if p.poll() is not None:
                raise RuntimeError(f"server exited early rc={p.returncode}")
            time.sleep(0.25)
    raise RuntimeError("server did not become ready")


def stop_server(p):
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(timeout=15)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def pdf_text(pdf_bytes, name):
    f = TMP / name
    f.write_bytes(pdf_bytes)
    out = subprocess.run(["pdftotext", "-layout", str(f), "-"],
                         capture_output=True, text=True, check=True)
    return out.stdout


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


SECTIONS = (
    "## What worked",
    "## Craft points",
    "## A question for you",
    "## Follow-up exercise",
    "## Voice check",
)
BANDS = ("Reads human.", "Mixed.", "Reads machine-made.")


def voice_section(body):
    """Text after the '## Voice check' heading (empty string if absent)."""
    i = body.find("## Voice check")
    return "" if i < 0 else body[i + len("## Voice check"):]


def band_of(body):
    """First non-empty line of the Voice check section, stripped."""
    for line in voice_section(body).splitlines():
        if line.strip():
            return line.strip()
    return ""


PIECE = (
    "The kettle had been singing for four minutes before she noticed it. "
    "Mara lifted it off the burner with the sleeve of her sweater and set it on the "
    "windowsill, where the frost had drawn a map of a country neither of them had visited. "
    "Her brother was still talking about the road, the long white road, the way the snow "
    "erased the centre line somewhere past the second bridge. She poured the water and "
    "watched the tea darken and did not tell him that she had already packed. "
    "Outside, the light went the colour of old paper. He asked whether she wanted company "
    "and she said the drive was long and he said he knew and neither of them moved."
)

# Deliberately machine-flavoured fixture: stock connectives, generic summary tone, uniform rhythm.
MACHINE_PIECE = (
    "In today's fast-paced world, it is important to note that change is a constant companion. "
    "The protagonist embarks on a journey of self-discovery that serves as a testament to the "
    "resilience of the human spirit. Throughout the narrative, the author delves into themes of "
    "loss, hope, and transformation. Moreover, the setting plays a crucial role in shaping the "
    "character's emotional landscape. At the end of the day, the story reminds us that every "
    "challenge is an opportunity for growth, and that the journey itself is the destination."
)


def main():
    import importlib.util
    sys.path.insert(0, str(ROOT))
    from app.peak import peak_status  # pure function, no server needed

    shutil.rmtree(TMP, ignore_errors=True)
    TMP.mkdir(parents=True, exist_ok=True)
    if DB.exists():
        DB.unlink()

    print(f"server: uvicorn on {BASE}  db={DB}")
    proc = start_server()
    try:
        # ---- C1: PWA shell ------------------------------------------------
        st, hdrs, html = req("GET", "/")
        check("C1 GET / serves HTML", st == 200 and "text/html" in hdrs.get("content-type", ""), f"{st}")
        check("C1 manifest linked from HTML", "/static/manifest.webmanifest" in html)
        st, _, man = req("GET", "/static/manifest.webmanifest")
        manj = json.loads(man)
        check("C1 manifest is valid JSON PWA manifest",
              st == 200 and manj.get("start_url") == "/" and manj.get("display") == "standalone")
        st, swh, sw = req("GET", "/sw.js")
        check("C1 service worker served from root with scope /",
              st == 200 and swh.get("service-worker-allowed") == "/" and len(sw) > 100,
              f"{st} Service-Worker-Allowed={swh.get('service-worker-allowed')}")
        st, _, js = req("GET", "/static/app.js")
        check("C1 app.js registers the worker with scope /",
              "serviceWorker.register('/sw.js', { scope: '/' })" in js)
        check("C1 service worker precaches the app shell", "'/'" in sw and "/static/app.js" in sw)

        # ---- C2: exercise library ----------------------------------------
        st, _, ex_raw = req("GET", "/api/exercises")
        exercises = json.loads(ex_raw)["exercises"]
        types = sorted({e["type"] for e in exercises})
        check("C2 >=12 exercises seeded", len(exercises) >= 12, f"{len(exercises)} exercises")
        check("C2 >=5 exercise types", len(types) >= 5, str(types))
        required = {"scene", "dialogue", "character", "description", "flash fiction"}
        check("C2 required types present", required.issubset(set(types)))
        check("C2 each exercise has prompt + suggested length",
              all(e["prompt"].strip() and e["suggested_length"].strip() for e in exercises))
        dealt = []
        for _ in range(12):
            _, _, d = req("GET", "/api/exercises/deal")
            dealt.append(json.loads(d)["exercise"]["id"])
        check("C2 deal returns a real exercise and is random",
              set(dealt).issubset({e["id"] for e in exercises}) and len(set(dealt)) > 1,
              f"ids seen {sorted(set(dealt))}")

        # ---- C3: persistence across restart ------------------------------
        exercise_id = exercises[0]["id"]
        st, _, created = req("POST", "/api/pieces",
                             {"title": "The Long White Road", "body": PIECE,
                              "exercise_id": exercise_id})
        pid = json.loads(created)["piece"]["id"]
        check("C3 piece created", st == 200 and pid > 0, f"id={pid}")

        # ---- C5/C6 need more pieces --------------------------------------
        extra = []
        for i, (t, b) in enumerate([
            ("Harbour Lights", "The ferry horn sounded twice across the grey water and the gulls ignored it entirely."),
            ("Kitchen Table", "Two cups, one cold. The chair across from her stayed empty until the light changed."),
            ("Winter Errand", "He walked to the post office in the snow and forgot the letter on the counter."),
        ], start=2):
            st, _, c = req("POST", "/api/pieces", {"title": t, "body": b, "exercise_id": None})
            extra.append(json.loads(c)["piece"]["id"])

        # ---- C4: real review (live provider) -----------------------------
        t0 = time.time()
        st, _, rev_raw = req("POST", f"/api/pieces/{pid}/review")
        review = json.loads(rev_raw)["review"]
        body = review["body"]
        idx = [body.find(h) for h in SECTIONS]
        check("C4 review stored with 5 sections in order",
              st == 200 and all(i >= 0 for i in idx) and idx == sorted(idx),
              f"positions {idx}")
        quotes = [q for q in re.findall(r"[\"“]([^\"”]{15,})[\"”]", body) if norm(q) in norm(PIECE)]
        check("C4 >=2 quoted lines from the piece", len(quotes) >= 2,
              f"{len(quotes)} verified quotes")
        check("C4 provider is live DeepSeek (not stub)",
              review["provider"] == "deepseek" and "STUB" not in body,
              f"provider={review['provider']} in {time.time()-t0:.1f}s")
        check("C4 Voice check band is one of the three verbatim lines",
              band_of(body) in BANDS, f"band={band_of(body)!r}")
        st, _, piece_raw = req("GET", f"/api/pieces/{pid}")
        stored = json.loads(piece_raw)
        check("C4 review persisted on the piece", len(stored["reviews"]) == 1
              and stored["reviews"][0]["provider"] == review["provider"])

        # ---- C1v/C2v: voice check on a deliberately machine-flavoured piece ----
        st, _, mv_raw = req("POST", "/api/pieces",
                            {"title": "Machine Flavour Fixture", "body": MACHINE_PIECE,
                             "exercise_id": None})
        mpid = json.loads(mv_raw)["piece"]["id"]
        t_v = time.time()
        st, _, mrev_raw = req("POST", f"/api/pieces/{mpid}/review")
        mreview = json.loads(mrev_raw)["review"]
        mbody = mreview["body"]
        midx = [mbody.find(h) for h in SECTIONS]
        check("C1v machine-flavoured review has 5 sections in order",
              st == 200 and all(i >= 0 for i in midx) and midx == sorted(midx),
              f"positions {midx}")
        mband = band_of(mbody)
        check("C1v machine-flavoured review band is valid", mband in BANDS, f"band={mband!r}")
        check("C1v machine-flavoured review is flagged, not passed as human",
              mband in ("Mixed.", "Reads machine-made."),
              f"band={mband!r} — a stock-phrased fixture must not read as human")
        vsection = voice_section(mbody)
        mquotes = [q for q in re.findall(r"[\"“]([^\"”]{15,})[\"”]", vsection)
                   if norm(q) in norm(MACHINE_PIECE)]
        check("C2v Voice check quotes at least one passage from the machine-flavoured piece",
              len(mquotes) >= 1,
              f"{len(mquotes)} verified quote(s); band={mband!r}; section={norm(vsection)[:200]}")
        check("C2v Voice check quotes at most three passages (prompt contract)",
              len(mquotes) <= 3, f"{len(mquotes)} verified quote(s)")
        print(f"\n--- Voice check on machine-flavoured fixture ({time.time()-t_v:.1f}s) ---")
        print(vsection.strip()[:600])
        print("----------------------------------------------------------\n")

        # ---- C3: no percentage / numeric AI score in any review output -------
        pct = re.compile(r"\b\d{1,3}\s?%")
        spelled = re.compile(r"\bpercent(?:age)?\b", re.IGNORECASE)
        scored = re.compile(r"\bAI\s*(?:score|detector|percentage)\b", re.IGNORECASE)
        offenders = []
        for name, text in (("human fixture", body), ("machine fixture", mbody)):
            m_pct = pct.search(text)
            m_spelled = spelled.search(text)
            m_score = scored.search(text)
            if m_pct:
                offenders.append(f"{name}: {m_pct.group(0)!r}")
            if m_spelled:
                offenders.append(f"{name}: {m_spelled.group(0)!r}")
            if m_score:
                offenders.append(f"{name}: {m_score.group(0)!r}")
        check("C3 no percentage or numeric AI score in the review outputs", not offenders,
              "; ".join(offenders) or "no percent-sign figures, no AI-score phrasing")

        # ---- Discussion flow end-to-end ----------------------------------
        rid = review["id"]
        disc_query = "Could you suggest how to make the kettle moment feel more immediate?"
        t_disc = time.time()
        st, _, disc_raw = req("POST", f"/api/reviews/{rid}/messages", {"text": disc_query})
        disc_data = json.loads(disc_raw)
        reply = disc_data.get("reply") or disc_data["messages"][1]
        reply_text = reply["text"]
        is_disc_stub = reply_text.startswith("**STUB")
        disc_provider = "stub" if is_disc_stub else "live DeepSeek"
        print(f"\n--- Discussion Reply ({disc_provider} in {time.time()-t_disc:.1f}s) ---")
        print(reply_text.strip()[:300] + ("..." if len(reply_text) > 300 else ""))
        print("----------------------------------------------------------\n")
        check("Discussion message posted and reply returned",
              st == 200 and len(reply_text) > 0,
              f"provider={disc_provider}")

        st, _, thread_raw = req("GET", f"/api/reviews/{rid}/messages")
        thread_messages = json.loads(thread_raw)["messages"]
        check("Discussion thread returned in order",
              st == 200 and len(thread_messages) >= 2
              and thread_messages[0]["role"] == "user"
              and thread_messages[0]["text"] == disc_query
              and thread_messages[1]["role"] == "assistant",
              f"{len(thread_messages)} messages in thread")

        # ---- C4v: a discussion message asking about the voice check -------
        mrid = mreview["id"]
        v_query = ("You said this reads machine-made. Which lines made you hear it that way, "
                   "and would you still say so if I told you I wrote it quickly and plainly?")
        t_vd = time.time()
        st, _, vd_raw = req("POST", f"/api/reviews/{mrid}/messages", {"text": v_query})
        vd = json.loads(vd_raw)
        vreply = (vd.get("reply") or vd["messages"][1])["text"]
        is_v_stub = vreply.startswith("**STUB")
        # live: must engage the flagged material — quote a flagged line, or name the voice check
        flagged_tokens = []
        for q in re.findall(r"[\"“]([^\"”]{15,})[\"”]", vsection):
            flagged_tokens += [w.lower().strip(".,;:") for w in q.split() if len(w) > 5]
        engaged = ("voice" in vreply.lower()
                   or any(tok in vreply.lower() for tok in flagged_tokens))
        check("C4v asking about the voice check gets an engaging live reply (or a STUB-marked one)",
              st == 200 and len(vreply) > 0 and (is_v_stub or engaged),
              f"provider={'stub' if is_v_stub else 'live'} in {time.time()-t_vd:.1f}s "
              f"engaged={engaged}")
        print(f"\n--- Voice-check discussion reply ---\n{vreply.strip()[:400]}\n"
              "----------------------------------------------------------\n")

        # ---- C5: single-piece export -------------------------------------
        st, hdrs, md = req("POST", f"/api/export/piece/{pid}",
                           {"format": "md", "include_reviews": True})
        check("C5 md export contains full text + review",
              st == 200 and norm(PIECE) in norm(md) and "## What worked" in md,
              f"{len(md)} bytes")
        check("C5 md export carries the Voice check section",
              "## Voice check" in md and band_of(md) in BANDS,
              f"band in export={band_of(md)!r}")
        st, hdrs, pdf = req("POST", f"/api/export/piece/{pid}",
                            {"format": "pdf", "include_reviews": True}, raw=True)
        txt = pdf_text(pdf, "single.pdf")
        check("C5 pdf export contains full text (pdftotext)",
              st == 200 and hdrs.get("content-type", "").startswith("application/pdf")
              and norm(PIECE) in norm(txt), f"{len(pdf)} bytes -> {len(txt)} chars")
        check("C5 pdf export carries the Voice check section (pdftotext)",
              "Voice check" in txt and any(b in txt for b in BANDS),
              f"band found={[b for b in BANDS if b in txt]}")
        st, _, md2 = req("POST", f"/api/export/piece/{pid}",
                         {"format": "md", "include_reviews": False})
        check("C5.1 piece-only toggle excludes reviews", "## What worked" not in md2)
        st, _, md3 = req("POST", f"/api/export/piece/{pid}",
                         {"format": "md", "include_reviews": True, "include_discussions": True})
        check("C5.2 export with include_discussions includes discussion thread",
              st == 200 and disc_query in md3 and "Discussion" in md3)

        # ---- C6: portfolio ------------------------------------------------
        req("PUT", "/api/settings", {"display_name": "Bethany"})
        order = [extra[2], pid, extra[0]]  # Winter Errand, The Long White Road, Harbour Lights
        st, _, pmd = req("POST", "/api/export/portfolio",
                         {"piece_ids": order, "format": "md", "include_reviews": False})
        pos = [pmd.find(t) for t in ("Winter Errand", "The Long White Road", "Harbour Lights")]
        others = [pmd.find(t) for t in ("Harbour Lights", "Kitchen Table")]
        check("C6 portfolio md: 3 pieces in chosen order",
              st == 200 and -1 < pos[0] < pos[1] < pos[2], f"positions {pos}")
        check("C6 portfolio md: title block carries display name",
              "Bethany" in pmd.split("Winter Errand")[0])
        check("C6 portfolio md: unselected piece (Kitchen Table) absent",
              "Kitchen Table" not in pmd)
        st, hdrs, ppdf = req("POST", "/api/export/portfolio",
                             {"piece_ids": order, "format": "pdf", "include_reviews": False},
                             raw=True)
        ptxt = pdf_text(ppdf, "portfolio.pdf")
        ppos = [ptxt.find(t) for t in ("Winter Errand", "The Long White Road", "Harbour Lights")]
        check("C6 portfolio pdf: 3 pieces in chosen order (pdftotext)",
              st == 200 and -1 < ppos[0] < ppos[1] < ppos[2], f"positions {ppos}")
        check("C6 portfolio pdf: title page shows display name",
              "Bethany" in ptxt.split("Winter Errand")[0])

        # ---- C8 live status ----------------------------------------------
        st, _, stat = req("GET", "/api/status")
        sj = json.loads(stat)["peak"]
        now = datetime.now(timezone.utc)
        expect = peak_status(now)
        check("C8 /api/status peak block matches the UTC clock",
              sj["is_peak"] == expect["is_peak"] and sj["next_flip_utc"] == expect["next_flip_utc"],
              f"now={now:%a %H:%M}Z -> {sj['label']} until {sj['next_flip_utc']}")

        # ---- C3 (restart) -------------------------------------------------
        stop_server(proc)
        proc = start_server()
        st, _, back = req("GET", f"/api/pieces/{pid}")
        pj = json.loads(back)
        check("C3 piece survives a server restart (SQLite)",
              st == 200 and pj["piece"]["title"] == "The Long White Road"
              and len(pj["reviews"]) == 1, f"{len(pj['reviews'])} review(s) kept")
        st, _, back_thread = req("GET", f"/api/reviews/{rid}/messages")
        bt = json.loads(back_thread)["messages"]
        check("C3 discussion thread survives a server restart (SQLite)",
              st == 200 and len(bt) >= 2 and bt[0]["text"] == disc_query,
              f"{len(bt)} message(s) kept")
    finally:
        stop_server(proc)

    # ---- C8 peak unit cases (fixed clock) -------------------------------
    cases = [
        (datetime(2026, 11, 15, 15, tzinfo=timezone.utc), False, "2026-11-16T01:00:00Z", "Sun 15:00Z"),
        (datetime(2026, 11, 17, 2, tzinfo=timezone.utc), True, "2026-11-17T04:00:00Z", "Tue 02:00Z"),
        (datetime(2026, 11, 17, 5, tzinfo=timezone.utc), False, "2026-11-17T06:00:00Z", "Tue 05:00Z"),
        (datetime(2026, 11, 20, 9, tzinfo=timezone.utc), True, "2026-11-20T10:00:00Z", "Fri 09:00Z"),
        (datetime(2026, 11, 20, 11, tzinfo=timezone.utc), False, "2026-11-23T01:00:00Z", "Fri 11:00Z"),
    ]
    rows = []
    for dt, is_peak, flip, label in cases:
        got = peak_status(dt)
        rows.append(f"{label}: {got['label']} next={got['next_flip_utc']} local={got['next_flip_local']}")
        check(f"C8 {label} -> {'peak' if is_peak else 'off-peak'} until {flip}",
              got["is_peak"] == is_peak and got["next_flip_utc"] == flip, rows[-1])

    failed = [r for r in results if not r[1]]
    print("\n--- peak table ---")
    print("\n".join(rows))
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
