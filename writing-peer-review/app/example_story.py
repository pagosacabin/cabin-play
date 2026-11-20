"""The built-in Example Story.

This is a fixture of the app, not one-off data: ``ensure_example_story`` runs on
every database initialisation so the piece exists after a restart, a fresh
database or a deploy. The row is marked ``pieces.is_fixture = 1``, which is what
makes the delete endpoint refuse it (the UI hides the Delete button for the same
reason). Renaming, editing or adding to the Example Story stays possible; only
deleting it is prevented.

The text below is the 0223 end-to-end material (Marta and Elias) kept verbatim,
plus the review that DeepSeek produced for it during 0224 — frozen here so a
fresh database seeds the same showcase without a provider call.
"""

from typing import Any

EXAMPLE_STORY_TITLE = "Example Story"

# Titles this piece may already carry from an older database. The 0223 end-to-end
# project was left live under this name; it is adopted and renamed, never copied.
LEGACY_EXAMPLE_STORY_TITLES = ("0223 E2E",)

EXAMPLE_STORY_BODY = "Marta kept the lighthouse ledger in a tin box under the stairs.\n\nShe counted the lamps twice, as she always did.\n\nElias had promised her the lamp would be repaired before the equinox, and he had not touched it in three weeks. She wrote the date beside his name anyway.\n\nThe storm came in from the north, and the light stayed dark until dawn.\n\nBy morning the gale had moved inland and the sea was flat and grey."

EXAMPLE_STORY_REVIEW = "## What worked\nThe piece has a real instinct for the weight of small, repeated gestures. \"She counted the lamps twice, as she always did\" tells me more about Marta's relationship to order and duty than a paragraph of explanation could, and the \"as she always did\" is doing quiet, efficient work. The detail of the tin box under the stairs places the ledger in a physical, slightly hidden world. And the ending lands because of its restraint: \"the sea was flat and grey\" after \"the light stayed dark until dawn\" lets the aftermath sit in the reader's lap without commentary. The gap between what Marta records (\"She wrote the date beside his name anyway\") and what she feels is the emotional engine of the whole piece.\n\n## Craft points\n1. **Pacing and the elision of the storm.** The jump from the ledger entry to \"The storm came in from the north\" is abrupt in a way that mostly works, but the storm itself gets one clause while the counting gets two. Consider whether you want the disaster to feel this compressed or whether a single sensory beat during the dark night (a sound, a cold, a hand on the rail) would make the dawn's flatness land harder by contrast.\n\n2. **Interiority and the withheld reaction.** We never get Marta's response to Elias's failure beyond the act of writing the date. That's a strong choice, but it means the reader has to infer whether she's resigned, furious, or protecting him. One small physical tell — the pen pressing too hard, the date written crooked — could deepen the subtext without breaking the restraint.\n\n3. **Sentence rhythm.** The prose is uniformly short and declarative, which suits the ledger-keeper's voice, but the evenness flattens the climax. Varying one sentence's length or cadence at the storm's arrival would give the piece a peak.\n\n## A question for you\nWhat does Marta believe writing the date beside Elias's name actually accomplishes — is it a record for someone else to read, a private accusation, or a habit she can't stop?\n\n## Follow-up exercise\nWrite a 100-word scene set the morning after, in which Marta opens the tin box again. Do not let her speak or explain her feelings. Instead, show her reaction entirely through one physical action with the ledger (a correction, a smudge, a page turned back) and one detail of the room around her. The constraint: the word \"Elias\" may not appear.\n\n## Voice check\nReads human.\n\nThe piece reads as human throughout; the plainness is deliberate and controlled rather than flat, and I found no passages that read machine-made."

EXAMPLE_STORY_REVIEW_PROVIDER = "deepseek"

EXAMPLE_STORY_BIBLE = [
    ("character", "Marta", "Keeps the lighthouse ledger; the keeper who stays."),
    ("character", "Elias", "Promised to repair the lamp before the equinox; has not."),
    ("decision", "The light stays dark", "The lamp is dark until dawn; this is canon."),
    ("world", "The north storm", "The storm arrives from the north on the night the lamp fails."),
]

EXAMPLE_STORY_THREAD_TITLE = "Storm chapter"

EXAMPLE_STORY_THREAD_MESSAGES = [
    ("user", "Who are the two people in this story, and what has one of them promised the other? Answer from what you know about this project."),
    ("assistant", "Marta and Elias. She's the lighthouse keeper — the one who stays, the one who keeps the ledger in that tin box under the stairs. He's the one who was supposed to fix the lamp.\n\nThe promise is specific: Elias told Marta the lamp would be repaired before the equinox. It wasn't. \"He had not touched it in three weeks,\" and she wrote the date beside his name anyway — which is the detail I keep coming back to. She's documenting the failure rather than confronting it.\n\nWhy are you asking — are you checking whether the setup reads clearly, or are you working on something about their relationship?"),
    ("user", "I disagree. I think the dark lamp is too obvious a symbol. Argue for it or give me an alternative."),
    ("assistant", "Fair challenge, but let me push back first. The problem isn't that the dark lamp is a symbol — it's that you've already defused it. You didn't write \"the lamp went out\"; you wrote \"the light stayed dark until dawn,\" and before that Marta writes the date \"beside his name anyway.\" The lamp isn't doing the symbolic work alone — the ledger is. A dark lamp is obvious. A woman recording a broken promise in a tin box is not. So the symbol you may actually be leaning on is the bookkeeping, and the lamp is just the thing being bookkept.\n\nIf you still want the lamp to carry less weight, my alternative is to make the failure smaller and stranger — not \"the light stayed dark\" but something like the lamp coming on late, or flickering, so the reader can't settle on what it means. Ambiguity does more work than darkness. But I'd argue against cutting the dark entirely, because the equinox deadline needs a consequence, and dawn is the cleanest one you've got. What's making you feel it's too on-the-nose — the darkness itself, or how early it lands?"),
]


def _row_value(row: Any, key: str):
    try:
        return row[key]
    except (IndexError, KeyError, TypeError):
        return None


def ensure_example_story(conn, now_str: str) -> int:
    """Create, adopt or repair the Example Story fixture. Returns its piece id.

    Idempotent: re-running it on a seeded database changes nothing. Missing parts
    (body, review, story-bible entries, Develop thread) are filled in; existing
    content is never overwritten, so edits made in the app survive.
    """
    row = conn.execute(
        "SELECT id, title, body FROM pieces WHERE is_fixture = 1 ORDER BY id LIMIT 1;"
    ).fetchone()
    if row is None:
        row = conn.execute(
            "SELECT id, title, body FROM pieces WHERE title = ? ORDER BY id LIMIT 1;",
            (EXAMPLE_STORY_TITLE,),
        ).fetchone()
    if row is None:
        placeholders = ", ".join("?" for _ in LEGACY_EXAMPLE_STORY_TITLES)
        row = conn.execute(
            f"SELECT id, title, body FROM pieces WHERE title IN ({placeholders}) ORDER BY id LIMIT 1;",
            LEGACY_EXAMPLE_STORY_TITLES,
        ).fetchone()

    if row is None:
        cur = conn.execute(
            """
            INSERT INTO pieces (title, body, exercise_id, created_at, updated_at, is_fixture)
            VALUES (?, ?, NULL, ?, ?, 1);
            """,
            (EXAMPLE_STORY_TITLE, EXAMPLE_STORY_BODY, now_str, now_str),
        )
        piece_id = cur.lastrowid
    else:
        piece_id = _row_value(row, "id")
        conn.execute(
            "UPDATE pieces SET title = ?, is_fixture = 1 WHERE id = ?;",
            (EXAMPLE_STORY_TITLE, piece_id),
        )
        body = _row_value(row, "body") or ""
        if not body.strip():
            conn.execute(
                "UPDATE pieces SET body = ?, updated_at = ? WHERE id = ?;",
                (EXAMPLE_STORY_BODY, now_str, piece_id),
            )

    has_review = conn.execute(
        "SELECT COUNT(*) AS cnt FROM reviews WHERE piece_id = ?;", (piece_id,)
    ).fetchone()["cnt"]
    if not has_review:
        conn.execute(
            "INSERT INTO reviews (piece_id, body, created_at, provider) VALUES (?, ?, ?, ?);",
            (piece_id, EXAMPLE_STORY_REVIEW, now_str, EXAMPLE_STORY_REVIEW_PROVIDER),
        )

    bible_count = conn.execute(
        "SELECT COUNT(*) AS cnt FROM bible_entries WHERE piece_id = ?;", (piece_id,)
    ).fetchone()["cnt"]
    if not bible_count:
        conn.executemany(
            """
            INSERT INTO bible_entries (piece_id, kind, name, detail, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?);
            """,
            [(piece_id, kind, name, detail, now_str, now_str) for kind, name, detail in EXAMPLE_STORY_BIBLE],
        )

    thread_count = conn.execute(
        "SELECT COUNT(*) AS cnt FROM develop_threads WHERE piece_id = ?;", (piece_id,)
    ).fetchone()["cnt"]
    if not thread_count:
        cur = conn.execute(
            "INSERT INTO develop_threads (piece_id, title, created_at, updated_at) VALUES (?, ?, ?, ?);",
            (piece_id, EXAMPLE_STORY_THREAD_TITLE, now_str, now_str),
        )
        thread_id = cur.lastrowid
        conn.executemany(
            "INSERT INTO develop_messages (thread_id, role, text, created_at) VALUES (?, ?, ?, ?);",
            [(thread_id, role, text, now_str) for role, text in EXAMPLE_STORY_THREAD_MESSAGES],
        )

    return piece_id
