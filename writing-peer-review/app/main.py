from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Literal

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from app.config import DEFAULT_DISPLAY_NAME, STATIC_DIR, get_db_path
from app.db import get_connection, get_setting, init_db, set_setting
from app.export import (
    render_piece_markdown,
    render_piece_pdf,
    render_portfolio_markdown,
    render_portfolio_pdf,
)
from app.peak import peak_status
from app.provider import discuss_review, review_piece


class CreatePieceRequest(BaseModel):
    title: str = ""
    body: str = ""
    exercise_id: int | None = None


class UpdatePieceRequest(BaseModel):
    title: str = ""
    body: str = ""


class CreateMessageRequest(BaseModel):
    text: str


class SettingsRequest(BaseModel):
    display_name: str


class ExportPieceRequest(BaseModel):
    format: Literal["md", "pdf"]
    include_reviews: bool = True
    include_discussions: bool = False


class ExportPortfolioRequest(BaseModel):
    piece_ids: list[int]
    format: Literal["md", "pdf"]
    include_reviews: bool = True


def create_app() -> FastAPI:
    app = FastAPI(title="Writing Peer Review")

    # Read WPR_DB at app-creation time and initialize DB
    db_path = get_db_path()
    app.state.db_path = db_path
    init_db(db_path)

    # Mount static assets
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

    @app.get("/")
    def index():
        index_file = STATIC_DIR / "index.html"
        return FileResponse(index_file, media_type="text/html")

    # The service worker must be served from the root so its default scope covers "/"
    # (a worker served from /static/ can only control /static/*, which would leave the
    # app shell at "/" uncached and the app not truly installable/offline-capable).
    @app.get("/sw.js")
    def service_worker():
        return FileResponse(
            STATIC_DIR / "sw.js",
            media_type="application/javascript",
            headers={"Service-Worker-Allowed": "/", "Cache-Control": "no-cache"},
        )

    @app.get("/api/status")
    def get_status(request: Request):
        status_info = peak_status()
        display_name = get_setting(
            "display_name", DEFAULT_DISPLAY_NAME, request.app.state.db_path
        )
        return {
            "peak": status_info,
            "display_name": display_name,
        }

    @app.get("/api/exercises")
    def get_exercises(request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            cur = conn.execute(
                """
                SELECT id, type, title, prompt, suggested_length
                FROM exercises
                ORDER BY type ASC, title ASC;
                """
            )
            exercises = [dict(row) for row in cur.fetchall()]
            return {"exercises": exercises}
        finally:
            conn.close()

    @app.get("/api/exercises/deal")
    def deal_exercise(request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            cur = conn.execute(
                """
                SELECT id, type, title, prompt, suggested_length
                FROM exercises
                ORDER BY RANDOM()
                LIMIT 1;
                """
            )
            row = cur.fetchone()
            if not row:
                raise HTTPException(status_code=404, detail="No exercises available")
            return {"exercise": dict(row)}
        finally:
            conn.close()

    @app.post("/api/pieces")
    def create_piece(payload: CreatePieceRequest, request: Request):
        now_str = datetime.now(timezone.utc).isoformat()
        title = payload.title.strip() or "Untitled Piece"
        conn = get_connection(request.app.state.db_path)
        try:
            with conn:
                cur = conn.execute(
                    """
                    INSERT INTO pieces (title, body, exercise_id, created_at, updated_at)
                    VALUES (?, ?, ?, ?, ?);
                    """,
                    (title, payload.body, payload.exercise_id, now_str, now_str),
                )
                piece_id = cur.lastrowid

            piece_row = conn.execute(
                "SELECT * FROM pieces WHERE id = ?;", (piece_id,)
            ).fetchone()
            piece = dict(piece_row)
            piece["word_count"] = len(piece["body"].split()) if piece["body"] else 0
            piece["review_count"] = 0
            return {"piece": piece}
        finally:
            conn.close()

    @app.get("/api/pieces")
    def list_pieces(request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            cur = conn.execute(
                """
                SELECT p.id, p.title, p.body, p.exercise_id, p.created_at, p.updated_at,
                       COUNT(r.id) AS review_count
                FROM pieces p
                LEFT JOIN reviews r ON r.piece_id = p.id
                GROUP BY p.id
                ORDER BY p.updated_at DESC;
                """
            )
            pieces = []
            for row in cur.fetchall():
                body_text = row["body"] or ""
                pieces.append(
                    {
                        "id": row["id"],
                        "title": row["title"],
                        "created_at": row["created_at"],
                        "updated_at": row["updated_at"],
                        "word_count": len(body_text.split()) if body_text else 0,
                        "review_count": row["review_count"],
                    }
                )
            return {"pieces": pieces}
        finally:
            conn.close()

    @app.get("/api/pieces/{piece_id}")
    def get_piece(piece_id: int, request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            p_cur = conn.execute("SELECT * FROM pieces WHERE id = ?;", (piece_id,))
            p_row = p_cur.fetchone()
            if not p_row:
                raise HTTPException(status_code=404, detail="Piece not found")

            piece = dict(p_row)
            piece["word_count"] = len(piece["body"].split()) if piece["body"] else 0

            r_cur = conn.execute(
                """
                SELECT id, piece_id, body, created_at, provider
                FROM reviews
                WHERE piece_id = ?
                ORDER BY id ASC;
                """,
                (piece_id,),
            )
            reviews = [dict(row) for row in r_cur.fetchall()]
            for rev in reviews:
                m_cur = conn.execute(
                    """
                    SELECT id, review_id, role, text, created_at
                    FROM messages
                    WHERE review_id = ?
                    ORDER BY id ASC;
                    """,
                    (rev["id"],),
                )
                rev["messages"] = [dict(m) for m in m_cur.fetchall()]
            return {"piece": piece, "reviews": reviews}
        finally:
            conn.close()

    @app.put("/api/pieces/{piece_id}")
    def update_piece(piece_id: int, payload: UpdatePieceRequest, request: Request):
        now_str = datetime.now(timezone.utc).isoformat()
        title = payload.title.strip() or "Untitled Piece"
        conn = get_connection(request.app.state.db_path)
        try:
            with conn:
                cur = conn.execute(
                    """
                    UPDATE pieces
                    SET title = ?, body = ?, updated_at = ?
                    WHERE id = ?;
                    """,
                    (title, payload.body, now_str, piece_id),
                )
                if cur.rowcount == 0:
                    raise HTTPException(status_code=404, detail="Piece not found")

            p_row = conn.execute(
                "SELECT * FROM pieces WHERE id = ?;", (piece_id,)
            ).fetchone()
            piece = dict(p_row)
            piece["word_count"] = len(piece["body"].split()) if piece["body"] else 0

            r_count_cur = conn.execute(
                "SELECT COUNT(*) AS count FROM reviews WHERE piece_id = ?;", (piece_id,)
            )
            piece["review_count"] = r_count_cur.fetchone()["count"]
            return {"piece": piece}
        finally:
            conn.close()

    @app.post("/api/pieces/{piece_id}/review")
    def review_piece_endpoint(piece_id: int, request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            p_cur = conn.execute("SELECT * FROM pieces WHERE id = ?;", (piece_id,))
            p_row = p_cur.fetchone()
            if not p_row:
                raise HTTPException(status_code=404, detail="Piece not found")

            piece = dict(p_row)
            exercise_prompt: str | None = None
            if piece.get("exercise_id"):
                ex_cur = conn.execute(
                    "SELECT prompt FROM exercises WHERE id = ?;", (piece["exercise_id"],)
                )
                ex_row = ex_cur.fetchone()
                if ex_row:
                    exercise_prompt = ex_row["prompt"]

            review_text, provider_name = review_piece(piece["body"], exercise_prompt)
            now_str = datetime.now(timezone.utc).isoformat()

            with conn:
                r_cur = conn.execute(
                    """
                    INSERT INTO reviews (piece_id, body, created_at, provider)
                    VALUES (?, ?, ?, ?);
                    """,
                    (piece_id, review_text, now_str, provider_name),
                )
                review_id = r_cur.lastrowid

            return {
                "review": {
                    "id": review_id,
                    "piece_id": piece_id,
                    "body": review_text,
                    "created_at": now_str,
                    "provider": provider_name,
                    "messages": [],
                }
            }
        finally:
            conn.close()

    @app.get("/api/reviews/{review_id}/messages")
    def get_review_messages(review_id: int, request: Request):
        conn = get_connection(request.app.state.db_path)
        try:
            r_cur = conn.execute("SELECT id FROM reviews WHERE id = ?;", (review_id,))
            if not r_cur.fetchone():
                raise HTTPException(status_code=404, detail="Review not found")

            m_cur = conn.execute(
                """
                SELECT id, review_id, role, text, created_at
                FROM messages
                WHERE review_id = ?
                ORDER BY id ASC;
                """,
                (review_id,),
            )
            messages = [dict(row) for row in m_cur.fetchall()]
            return {"messages": messages}
        finally:
            conn.close()

    @app.post("/api/reviews/{review_id}/messages")
    def create_review_message(
        review_id: int, payload: CreateMessageRequest, request: Request
    ):
        conn = get_connection(request.app.state.db_path)
        try:
            r_cur = conn.execute(
                """
                SELECT r.id, r.piece_id, r.body AS review_body,
                       p.body AS piece_body, p.exercise_id,
                       e.prompt AS exercise_prompt
                FROM reviews r
                JOIN pieces p ON p.id = r.piece_id
                LEFT JOIN exercises e ON e.id = p.exercise_id
                WHERE r.id = ?;
                """,
                (review_id,),
            )
            r_row = r_cur.fetchone()
            if not r_row:
                raise HTTPException(status_code=404, detail="Review not found")

            user_text = payload.text.strip()
            if not user_text:
                raise HTTPException(status_code=400, detail="Message text cannot be empty")

            now_str = datetime.now(timezone.utc).isoformat()

            # Existing thread
            m_cur = conn.execute(
                """
                SELECT id, review_id, role, text, created_at
                FROM messages
                WHERE review_id = ?
                ORDER BY id ASC;
                """,
                (review_id,),
            )
            thread_so_far = [dict(m) for m in m_cur.fetchall()]

            # Store user message
            with conn:
                u_cur = conn.execute(
                    """
                    INSERT INTO messages (review_id, role, text, created_at)
                    VALUES (?, 'user', ?, ?);
                    """,
                    (review_id, user_text, now_str),
                )
                user_msg_id = u_cur.lastrowid

            user_msg = {
                "id": user_msg_id,
                "review_id": review_id,
                "role": "user",
                "text": user_text,
                "created_at": now_str,
            }

            full_thread = thread_so_far + [user_msg]

            # Call provider
            reply_text, provider_name = discuss_review(
                piece_text=r_row["piece_body"],
                review_text=r_row["review_body"],
                thread=full_thread,
                exercise_prompt=r_row["exercise_prompt"],
            )

            asst_now_str = datetime.now(timezone.utc).isoformat()

            # Store assistant reply
            with conn:
                a_cur = conn.execute(
                    """
                    INSERT INTO messages (review_id, role, text, created_at)
                    VALUES (?, 'assistant', ?, ?);
                    """,
                    (review_id, reply_text, asst_now_str),
                )
                asst_msg_id = a_cur.lastrowid

            assistant_msg = {
                "id": asst_msg_id,
                "review_id": review_id,
                "role": "assistant",
                "text": reply_text,
                "created_at": asst_now_str,
                "provider": provider_name,
            }

            return {
                "messages": [user_msg, assistant_msg],
                "user_message": user_msg,
                "assistant_message": assistant_msg,
                "reply": assistant_msg,
                "provider": provider_name,
            }
        finally:
            conn.close()

    @app.get("/api/settings")
    def get_settings_endpoint(request: Request):
        display_name = get_setting(
            "display_name", DEFAULT_DISPLAY_NAME, request.app.state.db_path
        )
        return {"display_name": display_name}

    @app.put("/api/settings")
    def update_settings_endpoint(payload: SettingsRequest, request: Request):
        clean_name = payload.display_name.strip() or DEFAULT_DISPLAY_NAME
        set_setting("display_name", clean_name, request.app.state.db_path)
        return {"display_name": clean_name}

    @app.post("/api/export/piece/{piece_id}")
    def export_piece_endpoint(
        piece_id: int, payload: ExportPieceRequest, request: Request
    ):
        conn = get_connection(request.app.state.db_path)
        try:
            p_cur = conn.execute("SELECT * FROM pieces WHERE id = ?;", (piece_id,))
            p_row = p_cur.fetchone()
            if not p_row:
                raise HTTPException(status_code=404, detail="Piece not found")

            piece = dict(p_row)
            reviews = []
            if payload.include_reviews:
                r_cur = conn.execute(
                    "SELECT * FROM reviews WHERE piece_id = ? ORDER BY id ASC;",
                    (piece_id,),
                )
                reviews = [dict(r) for r in r_cur.fetchall()]
                if payload.include_discussions:
                    for rev in reviews:
                        m_cur = conn.execute(
                            "SELECT * FROM messages WHERE review_id = ? ORDER BY id ASC;",
                            (rev["id"],),
                        )
                        rev["messages"] = [dict(m) for m in m_cur.fetchall()]

            safe_title = "".join(c for c in piece["title"] if c.isalnum() or c in ("-", "_")).strip() or f"piece-{piece_id}"

            if payload.format == "md":
                content = render_piece_markdown(
                    piece, reviews, payload.include_reviews, payload.include_discussions
                )
                return Response(
                    content=content,
                    media_type="text/markdown; charset=utf-8",
                    headers={"Content-Disposition": f'attachment; filename="{safe_title}.md"'},
                )
            elif payload.format == "pdf":
                pdf_bytes = render_piece_pdf(
                    piece, reviews, payload.include_reviews, payload.include_discussions
                )
                return Response(
                    content=pdf_bytes,
                    media_type="application/pdf",
                    headers={"Content-Disposition": f'attachment; filename="{safe_title}.pdf"'},
                )
            else:
                raise HTTPException(status_code=400, detail="Unsupported format")
        finally:
            conn.close()

    @app.post("/api/export/portfolio")
    def export_portfolio_endpoint(
        payload: ExportPortfolioRequest, request: Request
    ):
        conn = get_connection(request.app.state.db_path)
        try:
            display_name = get_setting(
                "display_name", DEFAULT_DISPLAY_NAME, request.app.state.db_path
            )

            pieces_with_reviews: list[tuple[dict[str, Any], list[dict[str, Any]]]] = []
            for pid in payload.piece_ids:
                p_cur = conn.execute("SELECT * FROM pieces WHERE id = ?;", (pid,))
                p_row = p_cur.fetchone()
                if not p_row:
                    continue
                piece = dict(p_row)
                reviews = []
                if payload.include_reviews:
                    r_cur = conn.execute(
                        "SELECT * FROM reviews WHERE piece_id = ? ORDER BY id ASC;",
                        (pid,),
                    )
                    reviews = [dict(r) for r in r_cur.fetchall()]
                pieces_with_reviews.append((piece, reviews))

            safe_name = "".join(c for c in display_name if c.isalnum() or c in ("-", "_")).strip() or "portfolio"

            if payload.format == "md":
                content = render_portfolio_markdown(
                    pieces_with_reviews, display_name, payload.include_reviews
                )
                return Response(
                    content=content,
                    media_type="text/markdown; charset=utf-8",
                    headers={"Content-Disposition": f'attachment; filename="{safe_name}-portfolio.md"'},
                )
            elif payload.format == "pdf":
                pdf_bytes = render_portfolio_pdf(
                    pieces_with_reviews, display_name, payload.include_reviews
                )
                return Response(
                    content=pdf_bytes,
                    media_type="application/pdf",
                    headers={"Content-Disposition": f'attachment; filename="{safe_name}-portfolio.pdf"'},
                )
            else:
                raise HTTPException(status_code=400, detail="Unsupported format")
        finally:
            conn.close()

    return app


app = create_app()
