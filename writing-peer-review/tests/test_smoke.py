from datetime import datetime, timezone
import pytest
from fastapi.testclient import TestClient

from app.main import create_app
from app.peak import peak_status


# 1. App Shell and PWA Asset Delivery
def test_1_app_shell_and_pwa_assets(client: TestClient):
    # GET / returns 200 HTML that references manifest and registers service worker
    resp = client.get("/")
    assert resp.status_code == 200
    assert "text/html" in resp.headers.get("content-type", "")
    html = resp.text
    assert "manifest.webmanifest" in html
    assert "/static/app.js" in html

    # GET /static/manifest.webmanifest 200
    m_resp = client.get("/static/manifest.webmanifest")
    assert m_resp.status_code == 200

    # The service worker must be served from the root so its scope covers "/"
    sw_resp = client.get("/sw.js")
    assert sw_resp.status_code == 200
    assert "javascript" in sw_resp.headers.get("content-type", "")
    assert sw_resp.headers.get("service-worker-allowed") == "/"

    js_resp = client.get("/static/app.js")
    assert js_resp.status_code == 200
    assert "serviceWorker.register('/sw.js', { scope: '/' })" in js_resp.text


# 2. Seeded Exercises and Deal Random Pick
def test_2_exercises_library_and_deal(client: TestClient):
    resp = client.get("/api/exercises")
    assert resp.status_code == 200
    data = resp.json()
    exercises = data.get("exercises", [])
    assert len(exercises) >= 12

    types = {ex["type"] for ex in exercises}
    assert len(types) >= 5
    for req_type in ("scene", "dialogue", "character", "description", "flash fiction"):
        assert req_type in types

    # /api/exercises/deal returns one of them
    deal_resp = client.get("/api/exercises/deal")
    assert deal_resp.status_code == 200
    dealt = deal_resp.json().get("exercise")
    assert dealt is not None
    assert any(ex["id"] == dealt["id"] for ex in exercises)


# 3. Persistence across Fresh App Instances (same WPR_DB)
def test_3_persistence_across_app_instances(client: TestClient):
    create_payload = {
        "title": "Persistent Midnight Draft",
        "body": "The cold tea sat untouched beside the stack of unanswered letters.",
        "exercise_id": None,
    }
    create_resp = client.post("/api/pieces", json=create_payload)
    assert create_resp.status_code == 200
    piece_id = create_resp.json()["piece"]["id"]

    # Open a new app instance and fresh TestClient
    fresh_app = create_app()
    with TestClient(fresh_app) as fresh_client:
        get_resp = fresh_client.get(f"/api/pieces/{piece_id}")
        assert get_resp.status_code == 200
        data = get_resp.json()
        assert data["piece"]["id"] == piece_id
        assert data["piece"]["title"] == "Persistent Midnight Draft"
        assert "The cold tea sat untouched" in data["piece"]["body"]


# 4. Review Submission with Monkeypatched Provider
def test_4_review_submission_monkeypatched(client: TestClient, monkeypatch):
    piece_body = (
        "The yellow kettle hissed loudly on the iron stove.\n"
        "She turned the brass valve and watched the white steam vanish into the rafters.\n"
        "Outside, the winter dusk settled without a word."
    )
    create_resp = client.post(
        "/api/pieces",
        json={"title": "The Iron Stove", "body": piece_body, "exercise_id": None},
    )
    assert create_resp.status_code == 200
    piece_id = create_resp.json()["piece"]["id"]

    # Deterministic monkeypatched provider returning exact 5 headings + >=2 quoted lines from input
    def fake_review_piece(piece_text: str, exercise_prompt: str | None) -> tuple[str, str]:
        assert "yellow kettle hissed loudly" in piece_text
        fake_content = (
            "## What worked\n"
            'I was immediately drawn to "The yellow kettle hissed loudly on the iron stove." because '
            'of the sensory immediacy. Furthermore, "She turned the brass valve and watched the white steam vanish into the rafters." '
            'creates a palpable moment of breath and suspension.\n\n'
            "## Craft points\n"
            "1. Sensory focus: The acoustic quality of the steam grounds the scene nicely.\n"
            "2. Sentence pacing: The short final sentence lands with quiet weight.\n\n"
            "## A question for you\n"
            "What memory did the sound of the steam bring up for her in this stillness?\n\n"
            "## Follow-up exercise\n"
            "Write a 75-word micro-scene in which an object in the kitchen stops working abruptly.\n\n"
            "## Voice check\n"
            "Reads human.\n"
            "The rhythm here is uneven in a way that sounds like a person listening for the next "
            "sentence rather than filling a shape."
        )
        return (fake_content, "deepseek-mock")

    monkeypatch.setattr("app.main.review_piece", fake_review_piece)

    review_resp = client.post(f"/api/pieces/{piece_id}/review")
    assert review_resp.status_code == 200
    review_data = review_resp.json()["review"]
    rev_body = review_data["body"]

    # Assert five headings in exact order
    h1 = rev_body.find("## What worked")
    h2 = rev_body.find("## Craft points")
    h3 = rev_body.find("## A question for you")
    h4 = rev_body.find("## Follow-up exercise")
    h5 = rev_body.find("## Voice check")
    assert -1 < h1 < h2 < h3 < h4 < h5

    # Assert the Voice check's first non-empty line is exactly one of the three bands
    band_lines = [
        ln.strip()
        for ln in rev_body[h5 + len("## Voice check"):].splitlines()
        if ln.strip()
    ]
    assert band_lines and band_lines[0] in ("Reads human.", "Mixed.", "Reads machine-made."), band_lines[:1]

    # Assert >= 2 quoted lines from piece text
    quote1 = '"The yellow kettle hissed loudly on the iron stove."'
    quote2 = '"She turned the brass valve and watched the white steam vanish into the rafters."'
    assert quote1 in rev_body
    assert quote2 in rev_body


def test_4b_stub_review_carries_voice_check(client: TestClient, monkeypatch):
    """With no API key the stub review still carries the five-section shape and a valid band."""
    piece_body = "The lantern guttered once on the sill and then held steady."
    create_resp = client.post(
        "/api/pieces",
        json={"title": "Lantern", "body": piece_body, "exercise_id": None},
    )
    assert create_resp.status_code == 200
    piece_id = create_resp.json()["piece"]["id"]

    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")

    review_resp = client.post(f"/api/pieces/{piece_id}/review")
    assert review_resp.status_code == 200
    review_data = review_resp.json()["review"]
    rev_body = review_data["body"]

    assert review_data["provider"] == "stub"
    assert rev_body.startswith("**STUB REVIEW**")

    idx = [rev_body.find(h) for h in ("## What worked", "## Craft points",
                                      "## A question for you", "## Follow-up exercise",
                                      "## Voice check")]
    assert all(i >= 0 for i in idx) and idx == sorted(idx), idx

    band_lines = [
        ln.strip()
        for ln in rev_body[idx[-1] + len("## Voice check"):].splitlines()
        if ln.strip()
    ]
    assert band_lines and band_lines[0] in ("Reads human.", "Mixed.", "Reads machine-made."), band_lines[:1]
    # The stub must never present itself as a live read
    assert "stub" in rev_body.lower()
    assert "not a live read" in rev_body.lower()
    assert "placeholder" in rev_body.lower()


def test_4c_band_is_normalised_to_its_own_line():
    """A live model sometimes continues on the band's line; the provider splits it."""
    from app.provider import normalize_voice_check

    merged = (
        "## Follow-up exercise\nDo the thing.\n\n"
        "## Voice check\n"
        'Reads machine-made. A few passages carry that generic quality: "in today\'s '
        'fast-paced world" is stock opener phrasing.'
    )
    fixed = normalize_voice_check(merged)
    lines = [ln for ln in fixed.split("## Voice check", 1)[1].splitlines() if ln.strip()]
    assert lines[0] == "Reads machine-made.", lines[:2]
    assert lines[1].startswith("A few passages carry"), lines[:2]

    head = merged.split("## Voice check")[0]
    for band in ("Reads human.", "Mixed.", "Reads machine-made."):
        out = normalize_voice_check(f"{head}## Voice check\n{band} Extra.")
        tail = [ln for ln in out.split("## Voice check", 1)[1].splitlines() if ln.strip()]
        assert tail == [band, "Extra."], (band, tail)

    well_formed = (
        "## Follow-up exercise\nDo the thing.\n\n"
        "## Voice check\nReads human.\nNothing here reads machine-made.\n"
    )
    assert normalize_voice_check(well_formed) == well_formed

    no_section = "## What worked\nFine piece.\n"
    assert normalize_voice_check(no_section) == no_section

    # Markdown emphasis on the explanation must survive intact (no dangling closer).
    emph = "## Voice check\nReads machine-made. **Generic** phrasing runs through it."
    out = normalize_voice_check(emph)
    assert out.split("## Voice check", 1)[1].strip().splitlines()[2] == \
        "**Generic** phrasing runs through it.", out

    # A quoted *mention* of the heading must not be mistaken for the heading.
    mention = (
        "## Craft points\nYou may wonder why I added a \"## Voice check\" section at all.\n\n"
        "## Voice check\nReads human.\nNothing reads machine-made here.\n"
    )
    assert normalize_voice_check(mention) == mention


# 5. Single Piece Export (md and pdf)
def test_5_single_piece_export(client: TestClient):
    body_text = "The cedar floorboards creaked softly under her boots as she crossed the threshold."
    create_resp = client.post(
        "/api/pieces",
        json={"title": "The Threshold Crossing", "body": body_text, "exercise_id": None},
    )
    piece_id = create_resp.json()["piece"]["id"]

    # Markdown export
    md_resp = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "md", "include_reviews": True},
    )
    assert md_resp.status_code == 200
    assert "text/markdown" in md_resp.headers.get("content-type", "")
    assert "The Threshold Crossing" in md_resp.text
    assert body_text in md_resp.text

    # PDF export
    pdf_resp = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "pdf", "include_reviews": True},
    )
    assert pdf_resp.status_code == 200
    assert "application/pdf" in pdf_resp.headers.get("content-type", "")
    pdf_content = pdf_resp.content.decode("latin-1")
    assert "The Threshold Crossing" in pdf_content
    assert body_text in pdf_content


# 6. Portfolio Export with Chosen Order and Display Name
def test_6_portfolio_export_ordering(client: TestClient):
    # Ensure display_name is Bethany
    client.put("/api/settings", json={"display_name": "Bethany"})

    # Create 3 distinct pieces
    p1 = client.post("/api/pieces", json={"title": "Story Alpha", "body": "Alpha content text."}).json()["piece"]
    p2 = client.post("/api/pieces", json={"title": "Story Beta", "body": "Beta content text."}).json()["piece"]
    p3 = client.post("/api/pieces", json={"title": "Story Gamma", "body": "Gamma content text."}).json()["piece"]

    # Choose a specific custom order: Gamma (p3), Alpha (p1), Beta (p2)
    chosen_order = [p3["id"], p1["id"], p2["id"]]

    # Markdown portfolio
    md_resp = client.post(
        "/api/export/portfolio",
        json={"piece_ids": chosen_order, "format": "md", "include_reviews": True},
    )
    assert md_resp.status_code == 200
    assert "text/markdown" in md_resp.headers.get("content-type", "")
    md_text = md_resp.text
    assert "Bethany" in md_text
    idx_gamma_md = md_text.find("Story Gamma")
    idx_alpha_md = md_text.find("Story Alpha")
    idx_beta_md = md_text.find("Story Beta")
    assert -1 < idx_gamma_md < idx_alpha_md < idx_beta_md

    # PDF portfolio
    pdf_resp = client.post(
        "/api/export/portfolio",
        json={"piece_ids": chosen_order, "format": "pdf", "include_reviews": True},
    )
    assert pdf_resp.status_code == 200
    assert "application/pdf" in pdf_resp.headers.get("content-type", "")
    pdf_text = pdf_resp.content.decode("latin-1")
    assert "Bethany" in pdf_text
    idx_gamma_pdf = pdf_text.find("Story Gamma")
    idx_alpha_pdf = pdf_text.find("Story Alpha")
    idx_beta_pdf = pdf_text.find("Story Beta")
    assert -1 < idx_gamma_pdf < idx_alpha_pdf < idx_beta_pdf


# 8. Peak Function: 5 Fixed UTC Test Cases & next_flip Correctness
def test_8_peak_function_cases():
    test_cases = [
        # (UTC datetime, expected is_peak, expected next_flip_utc string)
        (
            datetime(2026, 11, 15, 15, 0, tzinfo=timezone.utc),
            False,
            "2026-11-16T01:00:00Z",  # Sunday 15:00 UTC -> off-peak, next flip Monday 01:00 UTC
        ),
        (
            datetime(2026, 11, 17, 2, 0, tzinfo=timezone.utc),
            True,
            "2026-11-17T04:00:00Z",   # Tuesday 02:00 UTC -> peak, next flip Tuesday 04:00 UTC
        ),
        (
            datetime(2026, 11, 17, 5, 0, tzinfo=timezone.utc),
            False,
            "2026-11-17T06:00:00Z",  # Tuesday 05:00 UTC -> off-peak, next flip Tuesday 06:00 UTC
        ),
        (
            datetime(2026, 11, 20, 9, 0, tzinfo=timezone.utc),
            True,
            "2026-11-20T10:00:00Z",   # Friday 09:00 UTC -> peak, next flip Friday 10:00 UTC
        ),
        (
            datetime(2026, 11, 20, 11, 0, tzinfo=timezone.utc),
            False,
            "2026-11-23T01:00:00Z",  # Friday 11:00 UTC -> off-peak, next flip Monday 01:00 UTC (weekend is off-peak)
        ),
    ]

    for now_utc, expected_peak, expected_flip_iso in test_cases:
        res = peak_status(now_utc)
        assert res["is_peak"] == expected_peak
        assert res["label"] == ("Peak" if expected_peak else "Off-peak")
        assert res["next_flip_utc"] == expected_flip_iso
        assert "next_flip_local" in res
        assert ":" in res["next_flip_local"]
        assert ("AM" in res["next_flip_local"]) or ("PM" in res["next_flip_local"])
        assert "detail" in res


# 9. Discussion Stub Response and Persistence across App Re-opens
def test_9_discussion_stub_and_persistence(client: TestClient, monkeypatch):
    # Ensure no API key is set
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")

    # Create a piece
    create_resp = client.post(
        "/api/pieces",
        json={"title": "Stub Discussion Piece", "body": "Rain tapped against the pane."},
    )
    assert create_resp.status_code == 200
    piece_id = create_resp.json()["piece"]["id"]

    # Create a review
    review_resp = client.post(f"/api/pieces/{piece_id}/review")
    assert review_resp.status_code == 200
    review_id = review_resp.json()["review"]["id"]

    # 404 tests for unknown review id
    assert client.get("/api/reviews/99999/messages").status_code == 404
    assert client.post("/api/reviews/99999/messages", json={"text": "Hello?"}).status_code == 404

    # Post a message to the review
    user_msg_text = "Can you elaborate on the second craft point?"
    msg_resp = client.post(
        f"/api/reviews/{review_id}/messages",
        json={"text": user_msg_text},
    )
    assert msg_resp.status_code == 200
    data = msg_resp.json()

    # Reply must be stub-marked starting with **STUB
    reply = data.get("reply") or data["messages"][1]
    assert reply["role"] == "assistant"
    assert reply["text"].startswith("**STUB")
    assert data["user_message"]["text"] == user_msg_text

    # Thread returned by GET in order
    get_resp = client.get(f"/api/reviews/{review_id}/messages")
    assert get_resp.status_code == 200
    thread = get_resp.json()["messages"]
    assert len(thread) == 2
    assert thread[0]["role"] == "user"
    assert thread[0]["text"] == user_msg_text
    assert thread[1]["role"] == "assistant"
    assert thread[1]["text"].startswith("**STUB")

    # Thread survives an app/DB re-open (simulate restart)
    fresh_app = create_app()
    with TestClient(fresh_app) as fresh_client:
        reopened_resp = fresh_client.get(f"/api/reviews/{review_id}/messages")
        assert reopened_resp.status_code == 200
        reopened_thread = reopened_resp.json()["messages"]
        assert len(reopened_thread) == 2
        assert reopened_thread[0]["text"] == user_msg_text
        assert reopened_thread[1]["text"].startswith("**STUB")


# 10. Separate Threads for Multiple Reviews on the Same Piece
def test_10_discussion_separate_threads(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")

    # Create a single piece
    p_resp = client.post(
        "/api/pieces",
        json={"title": "Dual Reviews", "body": "Two cups on the wooden counter."},
    )
    piece_id = p_resp.json()["piece"]["id"]

    # Create two distinct reviews on this piece
    rev1_id = client.post(f"/api/pieces/{piece_id}/review").json()["review"]["id"]
    rev2_id = client.post(f"/api/pieces/{piece_id}/review").json()["review"]["id"]
    assert rev1_id != rev2_id

    # Post unique message to review 1
    msg1_text = "Message exclusively for review alpha"
    r1_post = client.post(f"/api/reviews/{rev1_id}/messages", json={"text": msg1_text})
    assert r1_post.status_code == 200

    # Post unique message to review 2
    msg2_text = "Message exclusively for review beta"
    r2_post = client.post(f"/api/reviews/{rev2_id}/messages", json={"text": msg2_text})
    assert r2_post.status_code == 200

    # Thread 1 contains msg1 and never msg2
    t1 = client.get(f"/api/reviews/{rev1_id}/messages").json()["messages"]
    t1_texts = [m["text"] for m in t1]
    assert any(msg1_text in t for t in t1_texts)
    assert not any(msg2_text in t for t in t1_texts)

    # Thread 2 contains msg2 and never msg1
    t2 = client.get(f"/api/reviews/{rev2_id}/messages").json()["messages"]
    t2_texts = [m["text"] for m in t2]
    assert any(msg2_text in t for t in t2_texts)
    assert not any(msg1_text in t for t in t2_texts)


# 11. Single-Piece Export with and without Discussions (.md and .pdf)
def test_11_export_with_and_without_discussions(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")

    piece_text = "The silver kettle began to hum at dawn across the quiet kitchen."
    p_resp = client.post(
        "/api/pieces",
        json={"title": "Kettle Song", "body": piece_text},
    )
    piece_id = p_resp.json()["piece"]["id"]

    rev_id = client.post(f"/api/pieces/{piece_id}/review").json()["review"]["id"]

    discussion_query = "Why did you suggest adding a tactile beat here?"
    msg_resp = client.post(
        f"/api/reviews/{rev_id}/messages",
        json={"text": discussion_query},
    )
    assert msg_resp.status_code == 200
    reply_text = msg_resp.json()["reply"]["text"]
    assert reply_text.startswith("**STUB")

    # (a) Export with include_discussions = True: thread text in both .md and .pdf
    md_with = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "md", "include_reviews": True, "include_discussions": True},
    )
    assert md_with.status_code == 200
    assert discussion_query in md_with.text
    assert "STUB DISCUSSION" in md_with.text

    pdf_with = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "pdf", "include_reviews": True, "include_discussions": True},
    )
    assert pdf_with.status_code == 200
    pdf_with_text = pdf_with.content.decode("latin-1")
    assert discussion_query in pdf_with_text
    assert "STUB DISCUSSION" in pdf_with_text

    # (b) Export with include_discussions = False: none of the thread text in .md or .pdf
    md_without = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "md", "include_reviews": True, "include_discussions": False},
    )
    assert md_without.status_code == 200
    assert discussion_query not in md_without.text
    assert "STUB DISCUSSION" not in md_without.text
    assert "## Peer Reviews" in md_without.text

    pdf_without = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "pdf", "include_reviews": True, "include_discussions": False},
    )
    assert pdf_without.status_code == 200
    pdf_without_text = pdf_without.content.decode("latin-1")
    assert discussion_query not in pdf_without_text
    assert "STUB DISCUSSION" not in pdf_without_text

    # (c) Export with include_reviews = False: neither reviews nor discussions present
    md_no_rev = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "md", "include_reviews": False, "include_discussions": True},
    )
    assert md_no_rev.status_code == 200
    assert discussion_query not in md_no_rev.text
    assert "## Peer Reviews" not in md_no_rev.text

    pdf_no_rev = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "pdf", "include_reviews": False, "include_discussions": True},
    )
    assert pdf_no_rev.status_code == 200
    pdf_no_rev_text = pdf_no_rev.content.decode("latin-1")
    assert discussion_query not in pdf_no_rev_text

    # (d) Backward compatibility: omit include_discussions completely
    md_compat = client.post(
        f"/api/export/piece/{piece_id}",
        json={"format": "md", "include_reviews": True},
    )
    assert md_compat.status_code == 200
    assert discussion_query not in md_compat.text
    assert "## Peer Reviews" in md_compat.text


# 12. discussion_prompt.md is re-read from disk on every request (not cached at import)
def test_12_discussion_prompt_reloaded_per_request(client: TestClient, monkeypatch):
    from app import provider as provider_mod
    from app.config import DISCUSSION_PROMPT_PATH

    captured: list[str] = []

    class _FakeResponse:
        def raise_for_status(self):
            return None

        def json(self):
            return {"choices": [{"message": {"content": "acknowledged"}}]}

    class _FakeClient:
        def __init__(self, *args, **kwargs):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def post(self, url, headers=None, json=None):  # noqa: A002 - mirror httpx API
            captured.append(json["messages"][0]["content"])
            return _FakeResponse()

    original_prompt = DISCUSSION_PROMPT_PATH.read_text(encoding="utf-8")
    monkeypatch.setattr(provider_mod, "get_deepseek_api_key", lambda: "test-key-not-used")
    monkeypatch.setattr(provider_mod.httpx, "Client", _FakeClient)
    try:
        p_resp = client.post(
            "/api/pieces",
            json={"title": "Prompt Reload", "body": "The lamp buzzed above the table."},
        )
        piece_id = p_resp.json()["piece"]["id"]
        rev_id = client.post(f"/api/pieces/{piece_id}/review").json()["review"]["id"]
        # the review call itself also goes through the patched httpx client; only the
        # two discussion requests below are under test
        captured.clear()

        marker_one = "PROMPT-MARKER-ALPHA-7f31"
        DISCUSSION_PROMPT_PATH.write_text(marker_one + "\n" + original_prompt, encoding="utf-8")
        r1 = client.post(f"/api/reviews/{rev_id}/messages", json={"text": "First question."})
        assert r1.status_code == 200
        assert r1.json()["provider"] == "deepseek"

        marker_two = "PROMPT-MARKER-BETA-9c02"
        DISCUSSION_PROMPT_PATH.write_text(marker_two + "\n" + original_prompt, encoding="utf-8")
        r2 = client.post(f"/api/reviews/{rev_id}/messages", json={"text": "Second question."})
        assert r2.status_code == 200
        assert r2.json()["provider"] == "deepseek"
    finally:
        DISCUSSION_PROMPT_PATH.write_text(original_prompt, encoding="utf-8")

    assert len(captured) == 2, captured
    assert "PROMPT-MARKER-ALPHA-7f31" in captured[0]
    assert "PROMPT-MARKER-BETA-9c02" not in captured[0]
    assert "PROMPT-MARKER-BETA-9c02" in captured[1]
    assert "PROMPT-MARKER-ALPHA-7f31" not in captured[1]
    # context order: prompt, then piece, then review, then thread
    assert captured[1].index("PROMPT-MARKER-BETA-9c02") < captured[1].index("The lamp buzzed")
    assert captured[1].index("The lamp buzzed") < captured[1].index("## Peer Review Under Discussion")


# 13. Two concurrent posts to the same review keep each reply paired with its own question
def test_13_concurrent_posts_do_not_interleave(client: TestClient, monkeypatch):
    import threading
    import time as _time

    from app import main as main_mod

    def fake_discuss(piece_text, review_text, thread, exercise_prompt=None):
        # slow enough that an unserialized implementation would overlap
        _time.sleep(0.25)
        return (f"reply to: {thread[-1]['text']}", "deepseek")

    monkeypatch.setattr(main_mod, "discuss_review", fake_discuss)

    p_resp = client.post("/api/pieces", json={"title": "Concurrency", "body": "Two lamps."})
    piece_id = p_resp.json()["piece"]["id"]
    rev_id = client.post(f"/api/pieces/{piece_id}/review").json()["review"]["id"]

    errors: list[BaseException] = []

    def post(text):
        try:
            client.post(f"/api/reviews/{rev_id}/messages", json={"text": text})
        except BaseException as exc:  # pragma: no cover - surfaced below
            errors.append(exc)

    threads = [threading.Thread(target=post, args=(t,)) for t in ("question-one", "question-two")]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)
    assert not errors, errors

    thread = client.get(f"/api/reviews/{rev_id}/messages").json()["messages"]
    roles = [m["role"] for m in thread]
    assert roles == ["user", "assistant", "user", "assistant"], roles
    # each assistant reply must belong to the question immediately before it
    for i in range(0, len(thread), 2):
        assert thread[i]["role"] == "user"
        assert thread[i + 1]["role"] == "assistant"
        assert thread[i + 1]["text"] == f"reply to: {thread[i]['text']}"


# 14. The service worker cache is versioned, so an installed PWA adopts the new shell
def test_14_service_worker_cache_version_bumped(client: TestClient):
    sw = client.get("/sw.js").text
    assert "writing-peer-review-v7" in sw
    assert "writing-peer-review-v6" not in sw
    assert "/static/app.js" in sw


# 15. DELETE /api/pieces/{piece_id} cascades to reviews and discussion messages,
# leaves unrelated pieces untouched, and 404s on missing ids.
def test_15_delete_piece_cascades_and_preserves_others(client: TestClient, monkeypatch):
    from app.db import get_connection

    # Ensure stub review provider is used so we don't need real network
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")

    # Create piece 1 with a review and discussion message
    p1_resp = client.post(
        "/api/pieces",
        json={"title": "Piece One To Delete", "body": "First piece body text for deletion."},
    )
    assert p1_resp.status_code == 200
    p1_id = p1_resp.json()["piece"]["id"]

    r1_resp = client.post(f"/api/pieces/{p1_id}/review")
    assert r1_resp.status_code == 200
    r1_id = r1_resp.json()["review"]["id"]

    m1_resp = client.post(f"/api/reviews/{r1_id}/messages", json={"text": "Question about piece 1"})
    assert m1_resp.status_code == 200

    # Create piece 2 with a review
    p2_resp = client.post(
        "/api/pieces",
        json={"title": "Piece Two To Keep", "body": "Second piece body text to stay safe."},
    )
    assert p2_resp.status_code == 200
    p2_id = p2_resp.json()["piece"]["id"]

    r2_resp = client.post(f"/api/pieces/{p2_id}/review")
    assert r2_resp.status_code == 200
    r2_id = r2_resp.json()["review"]["id"]

    # Verify rows exist in DB before delete
    db_path = client.app.state.db_path
    conn = get_connection(db_path)
    try:
        assert conn.execute("SELECT id FROM pieces WHERE id = ?;", (p1_id,)).fetchone() is not None
        assert conn.execute("SELECT id FROM reviews WHERE id = ?;", (r1_id,)).fetchone() is not None
        assert len(conn.execute("SELECT id FROM messages WHERE review_id = ?;", (r1_id,)).fetchall()) >= 1
        assert conn.execute("SELECT id FROM pieces WHERE id = ?;", (p2_id,)).fetchone() is not None
        assert conn.execute("SELECT id FROM reviews WHERE id = ?;", (r2_id,)).fetchone() is not None
    finally:
        conn.close()

    # (b) DELETE on non-existent id is 404
    non_existent_id = 987654321
    not_found_resp = client.delete(f"/api/pieces/{non_existent_id}")
    assert not_found_resp.status_code == 404
    assert not_found_resp.json() == {"detail": "Piece not found"}

    # (a) Delete piece 1 via endpoint
    del_resp = client.delete(f"/api/pieces/{p1_id}")
    assert del_resp.status_code == 200
    assert del_resp.json() == {"deleted": True, "piece_id": p1_id}

    # Verify endpoint GET returns 404 for piece 1 and its review messages
    assert client.get(f"/api/pieces/{p1_id}").status_code == 404
    assert client.get(f"/api/reviews/{r1_id}/messages").status_code == 404

    # Verify in DB: piece 1, review 1, and its messages are all gone via ON DELETE CASCADE
    conn = get_connection(db_path)
    try:
        assert conn.execute("SELECT id FROM pieces WHERE id = ?;", (p1_id,)).fetchone() is None
        assert conn.execute("SELECT id FROM reviews WHERE piece_id = ?;", (p1_id,)).fetchall() == []
        assert conn.execute("SELECT id FROM reviews WHERE id = ?;", (r1_id,)).fetchone() is None
        assert conn.execute("SELECT id FROM messages WHERE review_id = ?;", (r1_id,)).fetchall() == []

        # Piece 2 and its review are untouched
        p2_row = conn.execute("SELECT id, title FROM pieces WHERE id = ?;", (p2_id,)).fetchone()
        assert p2_row is not None
        assert p2_row["title"] == "Piece Two To Keep"
        r2_row = conn.execute("SELECT id, piece_id FROM reviews WHERE id = ?;", (r2_id,)).fetchone()
        assert r2_row is not None
        assert r2_row["piece_id"] == p2_id
    finally:
        conn.close()

    # Second piece and its review are accessible via HTTP
    get_p2 = client.get(f"/api/pieces/{p2_id}")
    assert get_p2.status_code == 200
    assert len(get_p2.json()["reviews"]) == 1
    assert get_p2.json()["reviews"][0]["id"] == r2_id

    # Deleting already-deleted piece 1 now 404s
    assert client.delete(f"/api/pieces/{p1_id}").status_code == 404


# 16. Portfolio export skips deleted / missing / stale piece ids without error
def test_16_portfolio_export_skips_stale_or_deleted_piece_ids(client: TestClient):
    # Ensure display name
    client.put("/api/settings", json={"display_name": "Bethany"})

    # Create two pieces
    p1 = client.post("/api/pieces", json={"title": "Ephemeral Draft", "body": "To be deleted soon."}).json()["piece"]
    p2 = client.post("/api/pieces", json={"title": "Permanent Draft", "body": "To be kept and exported."}).json()["piece"]

    # Delete piece 1
    del_resp = client.delete(f"/api/pieces/{p1['id']}")
    assert del_resp.status_code == 200

    # Request portfolio export containing stale deleted id and non-existent id along with valid id
    stale_order = [p1["id"], 9999999, p2["id"]]

    # Markdown export
    md_resp = client.post(
        "/api/export/portfolio",
        json={"piece_ids": stale_order, "format": "md", "include_reviews": False},
    )
    assert md_resp.status_code == 200
    assert "text/markdown" in md_resp.headers.get("content-type", "")
    assert "Permanent Draft" in md_resp.text
    assert "Ephemeral Draft" not in md_resp.text
    assert "**Total Pieces:** 1" in md_resp.text

    # PDF export
    pdf_resp = client.post(
        "/api/export/portfolio",
        json={"piece_ids": stale_order, "format": "pdf", "include_reviews": False},
    )
    assert pdf_resp.status_code == 200
    assert "application/pdf" in pdf_resp.headers.get("content-type", "")
    pdf_text = pdf_resp.content.decode("latin-1")
    assert "Permanent Draft" in pdf_text
    assert "Ephemeral Draft" not in pdf_text
    assert "Total Pieces: 1" in pdf_text

    # Completely empty or only stale ids still returns valid export (0 pieces) without 500 error
    md_empty = client.post(
        "/api/export/portfolio",
        json={"piece_ids": [p1["id"], 888888], "format": "md", "include_reviews": False},
    )
    assert md_empty.status_code == 200
    assert "**Total Pieces:** 0" in md_empty.text


# 17. The delete endpoint's cascade is the mechanism, not hand-rolled child deletes
#     (Copilot review finding on PR #4: test 15 alone cannot tell the two apart).
def test_17_delete_relies_on_fk_cascade_not_manual_child_deletes(client: TestClient, monkeypatch):
    import sqlite3

    from app.db import get_connection

    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    db_path = client.app.state.db_path

    # The app's own connection must enforce foreign keys, or the cascade never fires.
    conn = get_connection(db_path)
    try:
        assert conn.execute("PRAGMA foreign_keys;").fetchone()[0] == 1
    finally:
        conn.close()

    def seed(title):
        pid = client.post(
            "/api/pieces", json={"title": title, "body": f"{title} body text."}
        ).json()["piece"]["id"]
        rid = client.post(f"/api/pieces/{pid}/review").json()["review"]["id"]
        client.post(f"/api/reviews/{rid}/messages", json={"text": f"talk about {title}"})
        return pid, rid

    def child_rows(conn, pid):
        reviews = [r[0] for r in conn.execute("SELECT id FROM reviews WHERE piece_id = ?;", (pid,))]
        msgs = 0
        for rid in reviews:
            msgs += conn.execute("SELECT COUNT(*) FROM messages WHERE review_id = ?;", (rid,)).fetchone()[0]
        return reviews, msgs

    # Decisive discriminator: run the SAME endpoint over a connection with foreign_keys OFF.
    # A hand-rolled "delete the children too" implementation would still clear them; a single
    # `DELETE FROM pieces` relying on the FK cascade leaves them as orphans. So seeing the
    # children survive proves the endpoint does not touch the child tables itself.
    pid, rid = seed("Pragma Off Piece")
    real_get_connection = get_connection

    def fk_off(path=None):
        c = real_get_connection(path)
        c.execute("PRAGMA foreign_keys = OFF;")
        return c

    # Patch only the connection factory, and restore just that one: monkeypatch.undo() here
    # would also undo the no-live-API stub set above and let the positive case hit DeepSeek.
    import app.main as main_module

    original = main_module.get_connection
    main_module.get_connection = fk_off
    try:
        assert client.delete(f"/api/pieces/{pid}").status_code == 200
    finally:
        main_module.get_connection = original

    from app import provider as provider_module

    assert provider_module.get_deepseek_api_key() == "", "stub must still be in force for the positive case"

    raw = sqlite3.connect(str(db_path))
    try:
        assert raw.execute("SELECT COUNT(*) FROM pieces WHERE id = ?;", (pid,)).fetchone()[0] == 0
        reviews, msgs = child_rows(raw, pid)
        assert reviews == [rid], "endpoint deleted the reviews itself -- cascade is not the mechanism"
        assert msgs >= 1, "endpoint deleted the discussion messages itself"
        # clean the orphans so the session-scoped DB does not carry them into later work
        raw.execute("DELETE FROM messages WHERE review_id = ?;", (rid,))
        raw.execute("DELETE FROM reviews WHERE id = ?;", (rid,))
        raw.commit()
    finally:
        raw.close()

    # Positive case through the real (FK-enforcing) path: the same single delete clears all three.
    cascade_pid, cascade_rid = seed("Cascade Piece")
    assert client.delete(f"/api/pieces/{cascade_pid}").status_code == 200
    check = sqlite3.connect(str(db_path))
    try:
        assert child_rows(check, cascade_pid) == ([], 0)
        assert check.execute("SELECT COUNT(*) FROM messages WHERE review_id = ?;", (cascade_rid,)).fetchone()[0] == 0
    finally:
        check.close()


# =============================================================================
# DEVELOP MODE TESTS (new feature)
# =============================================================================


def _make_piece(client, body="The rain fell soft on the old stone roof."):
    """Helper: create a piece and return its id."""
    resp = client.post("/api/pieces", json={"title": "Test Piece", "body": body})
    assert resp.status_code == 200
    return resp.json()["piece"]["id"]


# 18. Develop thread create / list / delete
def test_18_develop_thread_create_list_delete(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    piece_id = _make_piece(client)

    # List threads (empty)
    list_resp = client.get(f"/api/pieces/{piece_id}/develop/threads")
    assert list_resp.status_code == 200
    assert list_resp.json()["threads"] == []

    # Create a thread
    create_resp = client.post(
        f"/api/pieces/{piece_id}/develop/threads",
        json={"title": "My first conversation"},
    )
    assert create_resp.status_code == 200
    thread = create_resp.json()["thread"]
    thread_id = thread["id"]
    assert thread["piece_id"] == piece_id
    assert thread["title"] == "My first conversation"
    assert "created_at" in thread

    # List threads now shows 1
    list_resp2 = client.get(f"/api/pieces/{piece_id}/develop/threads")
    assert list_resp2.status_code == 200
    threads = list_resp2.json()["threads"]
    assert len(threads) == 1
    assert threads[0]["id"] == thread_id
    assert "message_count" in threads[0]

    # Create a second thread with default title
    create_resp2 = client.post(
        f"/api/pieces/{piece_id}/develop/threads", json={}
    )
    assert create_resp2.status_code == 200
    assert create_resp2.json()["thread"]["title"] == "New conversation"

    # Delete first thread
    del_resp = client.delete(f"/api/develop/threads/{thread_id}")
    assert del_resp.status_code == 200
    assert del_resp.json() == {"deleted": True, "thread_id": thread_id}

    # List shows only 1 thread now
    list_resp3 = client.get(f"/api/pieces/{piece_id}/develop/threads")
    assert len(list_resp3.json()["threads"]) == 1
    assert list_resp3.json()["threads"][0]["id"] != thread_id

    # 404 on unknown thread
    assert client.delete("/api/develop/threads/999999").status_code == 404


# 19. Posting a message stores user+assistant pair and uses stub when no key
def test_19_develop_message_stub(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    piece_id = _make_piece(client)

    # Create thread
    tid = client.post(
        f"/api/pieces/{piece_id}/develop/threads", json={"title": "Chat"}
    ).json()["thread"]["id"]

    # Post a message
    msg_resp = client.post(
        f"/api/develop/threads/{tid}/messages",
        json={"text": "What should happen next in the scene?"},
    )
    assert msg_resp.status_code == 200
    data = msg_resp.json()
    messages = data["messages"]
    assert len(messages) == 2
    assert messages[0]["role"] == "user"
    assert messages[0]["text"] == "What should happen next in the scene?"
    assert messages[1]["role"] == "assistant"
    assert "STUB" in messages[1]["text"]  # stub reply is clearly marked
    assert data["provider"] == "stub"

    # GET the thread shows both messages persisted
    thread_resp = client.get(f"/api/develop/threads/{tid}")
    assert thread_resp.status_code == 200
    stored = thread_resp.json()["messages"]
    assert len(stored) == 2
    assert stored[0]["role"] == "user"
    assert stored[1]["role"] == "assistant"

    # Empty text is rejected
    empty_resp = client.post(
        f"/api/develop/threads/{tid}/messages", json={"text": ""}
    )
    assert empty_resp.status_code == 400


# 20. Bible create / list / update / delete
def test_20_bible_crud(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    piece_id = _make_piece(client)

    # List (empty)
    list_resp = client.get(f"/api/pieces/{piece_id}/bible")
    assert list_resp.status_code == 200
    assert list_resp.json()["entries"] == []

    # Create a character entry
    create_resp = client.post(
        f"/api/pieces/{piece_id}/bible",
        json={"kind": "character", "name": "Elena", "detail": "The protagonist; keeps a journal."},
    )
    assert create_resp.status_code == 200
    entry = create_resp.json()["entry"]
    entry_id = entry["id"]
    assert entry["kind"] == "character"
    assert entry["name"] == "Elena"
    assert entry["piece_id"] == piece_id

    # Create a decision entry
    client.post(
        f"/api/pieces/{piece_id}/bible",
        json={"kind": "decision", "name": "Ending", "detail": "She stays in the village."},
    )

    # List shows 2 entries
    list_resp2 = client.get(f"/api/pieces/{piece_id}/bible")
    assert list_resp2.status_code == 200
    entries = list_resp2.json()["entries"]
    assert len(entries) == 2

    # Update
    put_resp = client.put(
        f"/api/bible/{entry_id}",
        json={"kind": "character", "name": "Elena", "detail": "Updated detail."},
    )
    assert put_resp.status_code == 200
    assert put_resp.json()["entry"]["detail"] == "Updated detail."

    # Delete
    del_resp = client.delete(f"/api/bible/{entry_id}")
    assert del_resp.status_code == 200
    assert del_resp.json() == {"deleted": True, "entry_id": entry_id}

    # 404 on missing
    assert client.delete(f"/api/bible/{entry_id}").status_code == 404
    assert client.put(f"/api/bible/{entry_id}", json={"kind": "world", "name": "X", "detail": "Y"}).status_code == 404

    # List shows 1 entry
    list_resp3 = client.get(f"/api/pieces/{piece_id}/bible")
    assert len(list_resp3.json()["entries"]) == 1


# 21. propose-bible returns proposals and writes nothing to the bible
def test_21_propose_bible_no_writes(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    piece_id = _make_piece(client)

    tid = client.post(
        f"/api/pieces/{piece_id}/develop/threads", json={}
    ).json()["thread"]["id"]

    # Post a message so there is content
    client.post(
        f"/api/develop/threads/{tid}/messages",
        json={"text": "Let's settle that the main character is named Marco."},
    )

    # Call propose-bible
    propose_resp = client.post(f"/api/develop/threads/{tid}/propose-bible")
    assert propose_resp.status_code == 200
    data = propose_resp.json()
    assert "entries" in data
    assert "provider" in data
    # entries is a list (may be empty with stub)
    assert isinstance(data["entries"], list)

    # Bible is still empty — propose writes nothing
    bible_resp = client.get(f"/api/pieces/{piece_id}/bible")
    assert bible_resp.json()["entries"] == []


# 22. merge-preview writes nothing; merge-apply with confirmed=false is refused;
#     merge-apply with confirmed=true inserts bible entries and passage
def test_22_merge_preview_and_apply(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    body = "First paragraph of the story.\n\nSecond paragraph continues here.\n\nThird and final paragraph."
    piece_id = _make_piece(client, body=body)

    tid = client.post(
        f"/api/pieces/{piece_id}/develop/threads", json={}
    ).json()["thread"]["id"]
    client.post(
        f"/api/develop/threads/{tid}/messages",
        json={"text": "What if we add a beat about the wind?"},
    )

    # merge-preview writes nothing
    preview_resp = client.post(
        f"/api/develop/threads/{tid}/merge-preview",
        json={"insert_after_paragraph": "end"},
    )
    assert preview_resp.status_code == 200
    preview = preview_resp.json()
    assert "bible_entries" in preview
    assert "passage" in preview
    assert "insert_after_paragraph" in preview
    assert "provider" in preview

    # Piece body unchanged after preview
    piece_resp = client.get(f"/api/pieces/{piece_id}")
    assert piece_resp.json()["piece"]["body"] == body

    # merge-apply with confirmed=false is refused
    apply_no_confirm = client.post(
        f"/api/develop/threads/{tid}/merge-apply",
        json={
            "bible_entries": [],
            "passage": "A new passage.",
            "insert_after_paragraph": "end",
            "confirmed": False,
        },
    )
    assert apply_no_confirm.status_code == 400

    # Piece body still unchanged
    assert client.get(f"/api/pieces/{piece_id}").json()["piece"]["body"] == body

    # merge-apply with confirmed=true + "end" inserts at end
    apply_resp = client.post(
        f"/api/develop/threads/{tid}/merge-apply",
        json={
            "bible_entries": [{"kind": "world", "name": "Wind", "detail": "The wind picks up at dusk."}],
            "passage": "The wind swept through.",
            "insert_after_paragraph": "end",
            "confirmed": True,
        },
    )
    assert apply_resp.status_code == 200
    result = apply_resp.json()
    assert len(result["applied"]["bible_entries"]) == 1
    new_body = result["piece"]["body"]
    assert "The wind swept through." in new_body
    assert new_body.endswith("The wind swept through.")

    # Bible shows the new entry
    bible_entries = client.get(f"/api/pieces/{piece_id}/bible").json()["entries"]
    assert any(e["name"] == "Wind" for e in bible_entries)


def test_22b_merge_apply_at_paragraph_index(client: TestClient, monkeypatch):
    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    body = "Para zero.\n\nPara one.\n\nPara two."
    piece_id = _make_piece(client, body=body)
    tid = client.post(
        f"/api/pieces/{piece_id}/develop/threads", json={}
    ).json()["thread"]["id"]

    # Insert after paragraph 0 (0-based)
    apply_resp = client.post(
        f"/api/develop/threads/{tid}/merge-apply",
        json={
            "bible_entries": [],
            "passage": "Inserted passage.",
            "insert_after_paragraph": 0,
            "confirmed": True,
        },
    )
    assert apply_resp.status_code == 200
    new_body = apply_resp.json()["piece"]["body"]
    paragraphs = [p.strip() for p in new_body.split("\n\n") if p.strip()]
    # After para 0 we expect: [Para zero., Inserted passage., Para one., Para two.]
    assert paragraphs[0] == "Para zero."
    assert paragraphs[1] == "Inserted passage."
    assert paragraphs[2] == "Para one."


# 23. stuck returns directions and records a call; 4th call returns 429 and
#     makes no provider call and writes no stuck_calls row; a row older than
#     60 minutes does not count.
def test_23_stuck_cap_and_old_rows(client: TestClient, monkeypatch):
    from app.db import get_connection
    from datetime import timedelta

    monkeypatch.setattr("app.provider.get_deepseek_api_key", lambda: "")
    piece_id = _make_piece(client)

    # Insert an OLD stuck_calls row (> 60 minutes ago) directly into the DB
    db_path = client.app.state.db_path
    old_time = (datetime.now(timezone.utc) - timedelta(minutes=90)).isoformat()
    conn = get_connection(db_path)
    try:
        with conn:
            conn.execute(
                "INSERT INTO stuck_calls (piece_id, created_at) VALUES (?, ?);",
                (piece_id, old_time),
            )
    finally:
        conn.close()

    # Count of old row in DB before any new calls
    conn = get_connection(db_path)
    try:
        total_before = conn.execute(
            "SELECT COUNT(*) FROM stuck_calls WHERE piece_id = ?;", (piece_id,)
        ).fetchone()[0]
    finally:
        conn.close()
    assert total_before == 1  # the old row

    # 3 successful calls should work (old row does not count toward cap)
    for i in range(3):
        resp = client.post(f"/api/pieces/{piece_id}/stuck")
        assert resp.status_code == 200, f"call {i+1} failed: {resp.text}"
        data = resp.json()
        assert "directions" in data
        assert "provider" in data
        assert data["provider"] == "stub"
        assert "remaining" in data
        assert data["remaining"] == 2 - i  # 2, 1, 0

    # 4th call within the hour → 429, no provider call, no new DB row
    provider_called = []

    def fake_stuck_directions(piece_text):
        provider_called.append(piece_text)
        return ("1. Direction.\n2. Another.", "deepseek")

    monkeypatch.setattr("app.main.stuck_directions", fake_stuck_directions)

    conn = get_connection(db_path)
    try:
        count_before_4th = conn.execute(
            "SELECT COUNT(*) FROM stuck_calls WHERE piece_id = ?;", (piece_id,)
        ).fetchone()[0]
    finally:
        conn.close()

    resp4 = client.post(f"/api/pieces/{piece_id}/stuck")
    assert resp4.status_code == 429
    body4 = resp4.json()
    # The detail may be nested (FastAPI wraps HTTPException.detail)
    detail = body4.get("detail", {})
    if isinstance(detail, dict):
        assert detail.get("capped") is True
        assert detail.get("remaining") == 0
    else:
        # direct detail string
        assert "three" in str(detail).lower() or "capped" in str(body4).lower()

    # Provider was NOT called
    assert provider_called == [], "provider was called on the 4th (capped) request"

    # No new DB row was written
    conn = get_connection(db_path)
    try:
        count_after_4th = conn.execute(
            "SELECT COUNT(*) FROM stuck_calls WHERE piece_id = ?;", (piece_id,)
        ).fetchone()[0]
    finally:
        conn.close()
    assert count_after_4th == count_before_4th, "a stuck_calls row was written on a capped request"


# 24. Service worker cache version is now v6
def test_24_service_worker_cache_version_v7(client: TestClient):
    sw = client.get("/sw.js").text
    assert "writing-peer-review-v7" in sw
    assert "writing-peer-review-v6" not in sw


# 25. Extraction calls (propose-bible / merge-preview) must end on the
#     instruction, not on the thread's last assistant reply — a thread that ends
#     with an assistant turn makes the model continue the conversation instead
#     of answering in JSON (seen live 2026-11-19).
def test_25_extraction_calls_put_instruction_last(monkeypatch):
    import json as _json
    from app import provider as prov

    monkeypatch.setattr(prov, "get_deepseek_api_key", lambda: "test-key")
    thread = [
        {"role": "user", "text": "Who is Marta?"},
        {"role": "assistant", "text": "Marta keeps the ledger. What do you think?"},
    ]

    captured = []

    class FakeResponse:
        def __init__(self, content):
            self._content = content

        def raise_for_status(self):
            return None

        def json(self):
            return {"choices": [{"message": {"content": self._content}}]}

    class FakeClient:
        def __init__(self, *a, **k):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def post(self, url, headers=None, json=None, timeout=None):
            captured.append(json)
            last = json["messages"][-1]["content"]
            if "bible_entries" in last:
                return FakeResponse(_json.dumps({"bible_entries": [], "passage": "She waited."}))
            return FakeResponse(_json.dumps({"entries": [{"kind": "character", "name": "Marta", "detail": "Keeps the ledger."}]}))

    monkeypatch.setattr(prov.httpx, "Client", FakeClient)

    entries, provider_name = prov.propose_bible_entries("Body text.", [], thread)
    assert provider_name == "deepseek"
    assert entries and entries[0]["name"] == "Marta"
    merge, _ = prov.draft_merge("Body text.", [], thread, "end")
    assert merge["passage"] == "She waited."

    assert len(captured) == 2
    for payload in captured:
        assert payload["messages"][0]["role"] == "system"
        assert payload["messages"][-1]["role"] == "user"
        assert "STRICT JSON" in payload["messages"][-1]["content"]
        # the thread is rendered as transcript text, not as chat turns
        assert all(m["role"] == "system" or m is payload["messages"][-1] for m in payload["messages"])
        assert "Bethany: Who is Marta?" in payload["messages"][-1]["content"]


# 26. The app reports one version string, from one place, everywhere it is used.
def test_26_version_shown_and_reported(client: TestClient):
    from app.config import APP_VERSION

    # A real version number, not a placeholder.
    assert APP_VERSION.count(".") == 2
    assert all(part.isdigit() for part in APP_VERSION.split("."))

    assert client.get("/api/status").json()["version"] == APP_VERSION
    assert client.get("/api/settings").json()["version"] == APP_VERSION

    html = client.get("/").text
    assert 'id="app-version"' in html
    assert "Version" in html

    js = client.get("/static/app.js").text
    # The settings view fills the line from the API, and the header poll keeps it current.
    assert "appVersionEl.textContent = version" in js
    assert js.count("showVersion(data.version)") >= 2


# 27. The Example Story is a complete showcase: writing, a review, bible entries
#     and a Develop thread — all present without anyone opening the app first.
def test_27_example_story_is_a_complete_fixture(client: TestClient):
    pieces = client.get("/api/pieces").json()["pieces"]
    examples = [p for p in pieces if p.get("is_fixture")]
    assert len(examples) == 1, pieces

    ex = examples[0]
    assert ex["title"] == "Example Story"

    detail = client.get(f"/api/pieces/{ex['id']}").json()
    body = detail["piece"]["body"]
    assert "Marta" in body and "Elias" in body
    assert len(body.split()) >= 50

    assert len(detail["reviews"]) >= 1
    review = detail["reviews"][0]["body"]
    for heading in (
        "## What worked",
        "## Craft points",
        "## A question for you",
        "## Follow-up exercise",
        "## Voice check",
    ):
        assert heading in review

    bible = client.get(f"/api/pieces/{ex['id']}/bible").json()["entries"]
    kinds = {entry["kind"] for entry in bible}
    assert {"character", "decision"} <= kinds
    names = {entry["name"] for entry in bible}
    assert {"Marta", "Elias"} <= names

    threads = client.get(f"/api/pieces/{ex['id']}/develop/threads").json()["threads"]
    assert len(threads) >= 1
    thread_detail = client.get(f"/api/develop/threads/{threads[0]['id']}").json()
    roles = [m["role"] for m in thread_detail["messages"]]
    assert roles.count("user") >= 1 and roles.count("assistant") >= 1


# 28. Delete protection is server-side: the endpoint refuses the fixture even when
#     a client ignores the hidden button, and leaves every part of it intact.
def test_28_example_story_delete_refused_server_side(client: TestClient):
    from app.db import get_connection

    ex = next(
        p for p in client.get("/api/pieces").json()["pieces"] if p.get("is_fixture")
    )

    refused = client.delete(f"/api/pieces/{ex['id']}")
    assert refused.status_code == 403
    assert "can't be deleted" in refused.json()["detail"]

    # Still there, with its review, bible and thread untouched.
    still = client.get(f"/api/pieces/{ex['id']}")
    assert still.status_code == 200
    assert len(still.json()["reviews"]) >= 1
    assert len(client.get(f"/api/pieces/{ex['id']}/bible").json()["entries"]) >= 1

    # Every other piece stays deletable, as today.
    ordinary = client.post(
        "/api/pieces", json={"title": "Delete Me", "body": "A throwaway draft."}
    ).json()["piece"]
    assert client.delete(f"/api/pieces/{ordinary['id']}").status_code == 200
    assert client.get(f"/api/pieces/{ordinary['id']}").status_code == 404

    # And the refusal did not quietly drop the row.
    conn = get_connection(client.app.state.db_path)
    try:
        row = conn.execute(
            "SELECT id, is_fixture FROM pieces WHERE id = ?;", (ex["id"],)
        ).fetchone()
        assert row is not None and row["is_fixture"] == 1
    finally:
        conn.close()


# 29. A restart (or a deploy) re-runs init_db: the fixture must be repaired, not
#     duplicated, and nothing about it may change on a second pass.
def test_29_example_story_survives_reinit(client: TestClient):
    before_pieces = client.get("/api/pieces").json()["pieces"]
    ex_before = next(p for p in before_pieces if p.get("is_fixture"))
    bible_before = len(client.get(f"/api/pieces/{ex_before['id']}/bible").json()["entries"])
    threads_before = len(
        client.get(f"/api/pieces/{ex_before['id']}/develop/threads").json()["threads"]
    )
    reviews_before = len(client.get(f"/api/pieces/{ex_before['id']}").json()["reviews"])

    app2 = create_app()
    with TestClient(app2) as client2:
        after_pieces = client2.get("/api/pieces").json()["pieces"]
        examples = [p for p in after_pieces if p.get("is_fixture")]
        assert len(examples) == 1
        assert examples[0]["id"] == ex_before["id"]
        assert examples[0]["title"] == "Example Story"
        assert len(after_pieces) == len(before_pieces)

        after_detail = client2.get(f"/api/pieces/{ex_before['id']}").json()
        assert len(after_detail["reviews"]) == reviews_before
        assert len(
            client2.get(f"/api/pieces/{ex_before['id']}/bible").json()["entries"]
        ) == bible_before
        assert len(
            client2.get(f"/api/pieces/{ex_before['id']}/develop/threads").json()["threads"]
        ) == threads_before


# 30. An older database arrives with no fixture flag and a live "0223 E2E" project:
#     init_db adds the column and adopts that piece instead of creating a second one.
def test_30_legacy_database_is_migrated_and_adopted(temp_db, tmp_path):
    import sqlite3

    legacy = tmp_path / "legacy.db"
    conn = sqlite3.connect(str(legacy))
    try:
        conn.executescript(
            """
            CREATE TABLE pieces (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT NOT NULL,
                body TEXT NOT NULL,
                exercise_id INTEGER NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            INSERT INTO pieces (title, body, created_at, updated_at)
            VALUES ('0223 E2E', 'Marta kept the lighthouse ledger in a tin box.', '2026-11-19', '2026-11-19');
            INSERT INTO pieces (title, body, created_at, updated_at)
            VALUES ('First piece', 'Her own writing.', '2026-11-18', '2026-11-18');
            """
        )
        conn.commit()
    finally:
        conn.close()

    from app.db import init_db, get_connection

    init_db(legacy)

    conn = get_connection(legacy)
    try:
        columns = {row["name"] for row in conn.execute("PRAGMA table_info(pieces);")}
        assert "is_fixture" in columns

        fixtures = conn.execute(
            "SELECT id, title, body FROM pieces WHERE is_fixture = 1;"
        ).fetchall()
        assert len(fixtures) == 1
        assert fixtures[0]["title"] == "Example Story"
        # The adopted piece keeps its own words.
        assert fixtures[0]["body"].startswith("Marta kept the lighthouse ledger")

        total = conn.execute("SELECT COUNT(*) AS cnt FROM pieces;").fetchone()["cnt"]
        assert total == 2

        first = conn.execute(
            "SELECT is_fixture FROM pieces WHERE title = 'First piece';"
        ).fetchone()
        assert first["is_fixture"] == 0
    finally:
        conn.close()
