import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from app.config import DEFAULT_DISPLAY_NAME, get_db_path
from app.exercises import SEED_EXERCISES
from app.example_story import ensure_example_story


def get_connection(db_path: Path | str | None = None) -> sqlite3.Connection:
    """Open and return a configured SQLite connection."""
    if db_path is None:
        path = get_db_path()
    else:
        path = Path(db_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON;")
    return conn


def init_db(db_path: Path | str | None = None) -> None:
    """Create database tables and seed exercises and settings if missing."""
    conn = get_connection(db_path)
    try:
        with conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS exercises (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    type TEXT NOT NULL,
                    title TEXT NOT NULL,
                    prompt TEXT NOT NULL,
                    suggested_length TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS pieces (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    title TEXT NOT NULL,
                    body TEXT NOT NULL,
                    exercise_id INTEGER NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    is_fixture INTEGER NOT NULL DEFAULT 0,
                    FOREIGN KEY (exercise_id) REFERENCES exercises(id) ON DELETE SET NULL
                );

                CREATE TABLE IF NOT EXISTS reviews (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    piece_id INTEGER NOT NULL,
                    body TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    provider TEXT NOT NULL,
                    FOREIGN KEY (piece_id) REFERENCES pieces(id) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    review_id INTEGER NOT NULL,
                    role TEXT NOT NULL,
                    text TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    FOREIGN KEY (review_id) REFERENCES reviews(id) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS settings (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS develop_threads (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    piece_id INTEGER NOT NULL REFERENCES pieces(id) ON DELETE CASCADE,
                    title TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_develop_threads_piece_id
                    ON develop_threads(piece_id);

                CREATE TABLE IF NOT EXISTS develop_messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    thread_id INTEGER NOT NULL REFERENCES develop_threads(id) ON DELETE CASCADE,
                    role TEXT NOT NULL,
                    text TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_develop_messages_thread_id
                    ON develop_messages(thread_id);

                CREATE TABLE IF NOT EXISTS bible_entries (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    piece_id INTEGER NOT NULL REFERENCES pieces(id) ON DELETE CASCADE,
                    kind TEXT NOT NULL,
                    name TEXT NOT NULL,
                    detail TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_bible_entries_piece_id
                    ON bible_entries(piece_id);

                CREATE TABLE IF NOT EXISTS stuck_calls (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    piece_id INTEGER NOT NULL REFERENCES pieces(id) ON DELETE CASCADE,
                    created_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_stuck_calls_piece_id
                    ON stuck_calls(piece_id);
                """
            )

            # Default display name
            conn.execute(
                "INSERT OR IGNORE INTO settings (key, value) VALUES ('display_name', ?);",
                (DEFAULT_DISPLAY_NAME,),
            )

            # Older databases predate the fixture flag: add it without touching rows.
            piece_columns = {
                row["name"] for row in conn.execute("PRAGMA table_info(pieces);")
            }
            if "is_fixture" not in piece_columns:
                conn.execute(
                    "ALTER TABLE pieces ADD COLUMN is_fixture INTEGER NOT NULL DEFAULT 0;"
                )

            # Seed exercises if empty
            cursor = conn.execute("SELECT COUNT(*) AS count FROM exercises;")
            row = cursor.fetchone()
            if row and row["count"] == 0:
                conn.executemany(
                    """
                    INSERT INTO exercises (type, title, prompt, suggested_length)
                    VALUES (:type, :title, :prompt, :suggested_length);
                    """,
                    SEED_EXERCISES,
                )

            # The built-in Example Story is a fixture of every database, so a
            # restart, a fresh install or a deploy always has it to show off.
            ensure_example_story(conn, datetime.now(timezone.utc).isoformat())
    finally:
        conn.close()


def get_setting(key: str, default: str = "", db_path: Path | str | None = None) -> str:
    """Retrieve a setting value by key."""
    conn = get_connection(db_path)
    try:
        cur = conn.execute("SELECT value FROM settings WHERE key = ?;", (key,))
        row = cur.fetchone()
        return row["value"] if row else default
    finally:
        conn.close()


def set_setting(key: str, value: str, db_path: Path | str | None = None) -> None:
    """Set or update a setting value."""
    conn = get_connection(db_path)
    try:
        with conn:
            conn.execute(
                """
                INSERT INTO settings (key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value;
                """,
                (key, value),
            )
    finally:
        conn.close()
