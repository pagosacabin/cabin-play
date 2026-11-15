# Writing Peer Review (Rung 1)

A personal, local writing-exercise and peer-review tool. Deal a creative writing exercise, write in the browser with live word count and autosave, and receive structured peer feedback from an LLM that mirrors an observant writing-group peer, complete with a follow-up micro-exercise.

## One-Command Start

```bash
./run.sh
```

On a fresh clone the script creates `.venv/` and installs `requirements.txt` (first run only,
needs network); afterwards it starts instantly from the existing venv. Put your DeepSeek key in a
`.env` next to `run.sh` first (`DEEPSEEK_API_KEY=...`, mode 600) — without it the app still runs
and reviews fall back to the clearly-marked `STUB` provider.

It serves `http://127.0.0.1:8765` (override the port with `WPR_PORT`):
```bash
./.venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8765
```

## Where the Database Lives

The database is a single SQLite file located by default at:
```
data/app.db
```
The `data/` directory is created automatically on startup if missing. To override the database location (e.g. for testing), set the `WPR_DB` environment variable prior to starting the application or running tests.

## Tuning `review_prompt.md`

The peer review system prompt lives in plain Markdown at `review_prompt.md` at the repository root. It is re-read from disk on every review request, allowing you to edit the tone, constraints, and instructions without restarting the server.

The prompt requires the LLM to respond using four exact markdown headers:
- `## What worked`
- `## Craft points`
- `## A question for you`
- `## Follow-up exercise`

## Provider and API Key Notes

The application uses DeepSeek (`deepseek-chat` model by default) as its LLM review provider via the chat completions API (`https://api.deepseek.com/chat/completions`).

- The API key is stored strictly in the local, gitignored `.env` file (`mode 600`) as `DEEPSEEK_API_KEY`. It is never logged, printed, echoed, or committed.
- A lightweight, handwritten `.env` parser reads the environment without external dependencies.
- If the key is missing or an API call fails or times out, the app automatically falls back to a canned review marked clearly with provider `stub` and a leading `**STUB REVIEW**` line, ensuring requests never crash with a 500 error.

## PDF Library Choice

PDF rendering is built using Python's `reportlab` library (`SimpleDocTemplate`, `ParagraphStyle`, `PageBreak`, `HRFlowable`), generating clean, print-ready documents for single pieces and multi-piece portfolios with title pages. Stream compression is configured off (`rl_config.pageCompression = 0`) to allow direct text stream inspection.

## Important Architectural Notes

- **no auth in rung 1 — auth arrives with the VPS rung**: This rung is purely local and designed for single-user desktop/browser usage with zero authentication.
- **the Chinese-holiday exception to DeepSeek peak pricing is not implemented in rung 1**: DeepSeek peak pricing is calculated on a fixed schedule (01:00–04:00 and 06:00–10:00 UTC, Monday through Friday; weekends off-peak). Statutory Chinese holidays that DeepSeek treats as off-peak are not dynamically factored in for rung 1.
