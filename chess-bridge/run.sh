#!/usr/bin/env bash
# Start chess-bridge.  The DeepSeek key is read from DEEPSEEK_API_KEY, or
# extracted from ~/.hermes/.env when the variable is not already set; it is
# never printed and never copied into this tree.
set -euo pipefail

if [ -z "${DEEPSEEK_API_KEY:-}" ] && [ -r "$HOME/.hermes/.env" ]; then
  DEEPSEEK_API_KEY="$(sed -n 's/^DEEPSEEK_API_KEY=//p' "$HOME/.hermes/.env" | head -n 1 | tr -d '"'\''')"
  export DEEPSEEK_API_KEY
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HOME/.local/share/chess-bridge/venv/bin/python" "$HERE/server.py"
