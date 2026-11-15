#!/usr/bin/env bash
set -euo pipefail

# Start the Writing Peer Review application
# Uses existing local virtual environment without re-creating anything.
cd "$(dirname "$0")"
exec ./.venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8765
