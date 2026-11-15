#!/usr/bin/env bash
set -euo pipefail

# Start the Writing Peer Review app (rung 1, local only).
# First run: creates .venv and installs requirements.txt. Later runs: reuses .venv.
# Requires a .env in this directory holding DEEPSEEK_API_KEY (see README).

cd "$(dirname "$0")"

if [ ! -x .venv/bin/python ]; then
  echo "creating virtualenv in .venv ..."
  python3 -m venv .venv
  ./.venv/bin/python -m pip install --quiet --upgrade pip
  ./.venv/bin/python -m pip install --quiet -r requirements.txt
fi

if [ ! -f .env ]; then
  echo "warning: no .env here — reviews will fall back to the STUB provider." >&2
fi

exec ./.venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port "${WPR_PORT:-8765}"
