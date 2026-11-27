#!/usr/bin/env bash
# Run the Pocket Conquest bridge in the foreground. The systemd unit below does
# the same thing in the background.
#
# The DeepSeek key is read from the environment and must be exported here or in
# the unit's EnvironmentFile; it is never written into the repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "${DEEPSEEK_API_KEY:-}" ] && [ -r "$HOME/.hermes/.env" ]; then
  DEEPSEEK_API_KEY="$(sed -n 's/^DEEPSEEK_API_KEY=//p' "$HOME/.hermes/.env" | tail -n 1)"
  export DEEPSEEK_API_KEY
fi
: "${DEEPSEEK_API_KEY:?no DEEPSEEK_API_KEY and none in ~/.hermes/.env}"

exec python3 "$HERE/server.py"
