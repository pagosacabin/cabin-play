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
