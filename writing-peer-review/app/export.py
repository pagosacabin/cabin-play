import html
import io
from datetime import datetime, timezone
from typing import Any

from reportlab import rl_config
from reportlab.lib import colors
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.platypus import HRFlowable, PageBreak, Paragraph, SimpleDocTemplate, Spacer

# Ensure stream compression is disabled so rendered text is directly readable in test inspections
rl_config.pageCompression = 0


def render_piece_markdown(
    piece: dict[str, Any],
    reviews: list[dict[str, Any]],
    include_reviews: bool,
    include_discussions: bool = False,
) -> str:
    """Render a single piece as markdown."""
    title = piece.get("title", "Untitled")
    body = piece.get("body", "")
    created_at = piece.get("created_at", "")[:10]
    word_count = len(body.split()) if body else 0

    lines = [
        f"# {title}",
        "",
        f"**Date:** {created_at}  ",
        f"**Word count:** {word_count}  ",
        "",
        body,
    ]

    if include_reviews and reviews:
        lines.extend([
            "",
            "---",
            "",
            "## Peer Reviews",
            "",
        ])
        for idx, rev in enumerate(reviews, 1):
            r_created = rev.get("created_at", "")[:10]
            r_provider = rev.get("provider", "unknown")
            r_body = rev.get("body", "")
            lines.extend([
                f"### Review #{idx} ({r_provider} — {r_created})",
                "",
                r_body,
                "",
            ])
            if include_discussions and rev.get("messages"):
                lines.extend([
                    "#### Discussion",
                    "",
                ])
                for msg in rev["messages"]:
                    role = msg.get("role", "user")
                    role_display = "User" if role.lower() == "user" else "Assistant"
                    msg_text = msg.get("text", "")
                    lines.extend([
                        f"**{role_display}:** {msg_text}",
                        "",
                    ])

    return "\n".join(lines).strip() + "\n"


def render_portfolio_markdown(
    pieces_with_reviews: list[tuple[dict[str, Any], list[dict[str, Any]]]],
    display_name: str,
    include_reviews: bool,
) -> str:
    """Render a portfolio of pieces as markdown, starting with a title block."""
    now_str = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    lines = [
        f"# Writing Portfolio: {display_name}",
        "",
        f"**Author:** {display_name}  ",
        f"**Exported:** {now_str}  ",
        f"**Total Pieces:** {len(pieces_with_reviews)}  ",
        "",
        "---",
        "",
    ]

    piece_blocks = []
    for piece, reviews in pieces_with_reviews:
        piece_blocks.append(render_piece_markdown(piece, reviews, include_reviews))

    lines.append("\n\n---\n\n".join(piece_blocks))
    return "\n".join(lines).strip() + "\n"


def _build_pdf_styles():
    styles = getSampleStyleSheet()

    title_style = ParagraphStyle(
        "CoverTitle",
        parent=styles["Normal"],
        fontName="Helvetica-Bold",
        fontSize=24,
        leading=28,
        textColor=colors.HexColor("#1a1a1a"),
        spaceAfter=14,
    )
    piece_title_style = ParagraphStyle(
        "PieceTitle",
        parent=styles["Heading1"],
        fontName="Helvetica-Bold",
        fontSize=18,
        leading=22,
        textColor=colors.HexColor("#222222"),
        spaceAfter=8,
    )
    meta_style = ParagraphStyle(
        "MetaText",
        parent=styles["Normal"],
        fontName="Helvetica-Oblique",
        fontSize=10,
        leading=14,
        textColor=colors.HexColor("#555555"),
        spaceAfter=12,
    )
    body_style = ParagraphStyle(
        "BodySerif",
        parent=styles["Normal"],
        fontName="Times-Roman",
        fontSize=11,
        leading=16,
        textColor=colors.HexColor("#24201d"),
        spaceAfter=8,
    )
    section_heading_style = ParagraphStyle(
        "SectionHeading",
        parent=styles["Heading2"],
        fontName="Helvetica-Bold",
        fontSize=13,
        leading=17,
        textColor=colors.HexColor("#333333"),
        spaceBefore=12,
        spaceAfter=6,
    )
    review_body_style = ParagraphStyle(
        "ReviewBody",
        parent=styles["Normal"],
        fontName="Times-Italic",
        fontSize=10.5,
        leading=15,
        textColor=colors.HexColor("#2b2b2b"),
        spaceAfter=6,
    )

    return {
        "title": title_style,
        "piece_title": piece_title_style,
        "meta": meta_style,
        "body": body_style,
        "section": section_heading_style,
        "review": review_body_style,
    }


def render_piece_pdf(
    piece: dict[str, Any],
    reviews: list[dict[str, Any]],
    include_reviews: bool,
    include_discussions: bool = False,
) -> bytes:
    """Render a single piece as a PDF and return the raw bytes."""
    buf = io.BytesIO()
    doc = SimpleDocTemplate(
        buf,
        pagesize=letter,
        leftMargin=54,
        rightMargin=54,
        topMargin=54,
        bottomMargin=54,
    )
    styles = _build_pdf_styles()
    story = []

    title = piece.get("title", "Untitled")
    body = piece.get("body", "")
    created_at = piece.get("created_at", "")[:10]
    word_count = len(body.split()) if body else 0

    story.append(Paragraph(html.escape(title), styles["piece_title"]))
    story.append(
        Paragraph(f"Date: {html.escape(created_at)} &bull; Words: {word_count}", styles["meta"])
    )
    story.append(Spacer(1, 8))

    for paragraph in body.split("\n"):
        p_clean = paragraph.strip()
        if p_clean:
            story.append(Paragraph(html.escape(p_clean), styles["body"]))
            story.append(Spacer(1, 4))

    if include_reviews and reviews:
        story.append(Spacer(1, 14))
        story.append(HRFlowable(width="100%", thickness=0.5, color=colors.HexColor("#cccccc"), spaceBefore=10, spaceAfter=10))
        story.append(Paragraph("Peer Reviews", styles["section"]))
        for idx, rev in enumerate(reviews, 1):
            r_prov = rev.get("provider", "peer")
            r_date = rev.get("created_at", "")[:10]
            story.append(Paragraph(f"Review #{idx} &mdash; {html.escape(r_prov)} ({html.escape(r_date)})", styles["meta"]))
            for r_para in rev.get("body", "").split("\n"):
                rp_clean = r_para.strip()
                if rp_clean:
                    if rp_clean.startswith("## "):
                        story.append(Paragraph(html.escape(rp_clean[3:]), styles["section"]))
                    else:
                        story.append(Paragraph(html.escape(rp_clean), styles["review"]))
                    story.append(Spacer(1, 3))

            if include_discussions and rev.get("messages"):
                story.append(Spacer(1, 4))
                story.append(Paragraph("Discussion", styles["section"]))
                for msg in rev["messages"]:
                    role_str = html.escape("User" if msg.get("role", "user").lower() == "user" else "Assistant")
                    msg_text = html.escape(msg.get("text", ""))
                    story.append(Paragraph(f"<b>{role_str}:</b> {msg_text}", styles["body"]))
                    story.append(Spacer(1, 2))

    doc.build(story)
    return buf.getvalue()


def render_portfolio_pdf(
    pieces_with_reviews: list[tuple[dict[str, Any], list[dict[str, Any]]]],
    display_name: str,
    include_reviews: bool,
) -> bytes:
    """Render a portfolio of pieces as a PDF with a title page."""
    buf = io.BytesIO()
    doc = SimpleDocTemplate(
        buf,
        pagesize=letter,
        leftMargin=54,
        rightMargin=54,
        topMargin=54,
        bottomMargin=54,
    )
    styles = _build_pdf_styles()
    story = []

    # Title Page
    now_str = datetime.now(timezone.utc).strftime("%B %d, %Y")
    story.append(Spacer(1, 140))
    story.append(Paragraph(f"{html.escape(display_name)}'s Writing Portfolio", styles["title"]))
    story.append(Spacer(1, 8))
    story.append(Paragraph(f"Author: {html.escape(display_name)}", styles["meta"]))
    story.append(Paragraph(f"Date: {now_str}", styles["meta"]))
    story.append(Paragraph(f"Total Pieces: {len(pieces_with_reviews)}", styles["meta"]))
    story.append(PageBreak())

    # Pieces in exact order
    total = len(pieces_with_reviews)
    for idx, (piece, reviews) in enumerate(pieces_with_reviews):
        title = piece.get("title", "Untitled")
        body = piece.get("body", "")
        created_at = piece.get("created_at", "")[:10]
        word_count = len(body.split()) if body else 0

        story.append(Paragraph(html.escape(title), styles["piece_title"]))
        story.append(
            Paragraph(f"Date: {html.escape(created_at)} &bull; Words: {word_count}", styles["meta"])
        )
        story.append(Spacer(1, 8))

        for paragraph in body.split("\n"):
            p_clean = paragraph.strip()
            if p_clean:
                story.append(Paragraph(html.escape(p_clean), styles["body"]))
                story.append(Spacer(1, 4))

        if include_reviews and reviews:
            story.append(Spacer(1, 12))
            story.append(HRFlowable(width="100%", thickness=0.5, color=colors.HexColor("#cccccc"), spaceBefore=8, spaceAfter=8))
            story.append(Paragraph("Peer Reviews", styles["section"]))
            for r_idx, rev in enumerate(reviews, 1):
                r_prov = rev.get("provider", "peer")
                r_date = rev.get("created_at", "")[:10]
                story.append(Paragraph(f"Review #{r_idx} &mdash; {html.escape(r_prov)} ({html.escape(r_date)})", styles["meta"]))
                for r_para in rev.get("body", "").split("\n"):
                    rp_clean = r_para.strip()
                    if rp_clean:
                        if rp_clean.startswith("## "):
                            story.append(Paragraph(html.escape(rp_clean[3:]), styles["section"]))
                        else:
                            story.append(Paragraph(html.escape(rp_clean), styles["review"]))
                        story.append(Spacer(1, 3))

        if idx < total - 1:
            story.append(PageBreak())

    doc.build(story)
    return buf.getvalue()
