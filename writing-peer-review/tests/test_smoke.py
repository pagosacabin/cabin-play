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

    # Deterministic monkeypatched provider returning exact 4 headings + >=2 quoted lines from input
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
            "Write a 75-word micro-scene in which an object in the kitchen stops working abruptly."
        )
        return (fake_content, "deepseek-mock")

    monkeypatch.setattr("app.main.review_piece", fake_review_piece)

    review_resp = client.post(f"/api/pieces/{piece_id}/review")
    assert review_resp.status_code == 200
    review_data = review_resp.json()["review"]
    rev_body = review_data["body"]

    # Assert four headings in exact order
    h1 = rev_body.find("## What worked")
    h2 = rev_body.find("## Craft points")
    h3 = rev_body.find("## A question for you")
    h4 = rev_body.find("## Follow-up exercise")
    assert -1 < h1 < h2 < h3 < h4

    # Assert >= 2 quoted lines from piece text
    quote1 = '"The yellow kettle hissed loudly on the iron stove."'
    quote2 = '"She turned the brass valve and watched the white steam vanish into the rafters."'
    assert quote1 in rev_body
    assert quote2 in rev_body


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
