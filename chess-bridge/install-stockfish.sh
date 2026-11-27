#!/usr/bin/env bash
# Install the pinned Stockfish build this service expects.
#
# The tarball's SHA-256 is pinned: the release URL alone is not a pin, because
# the same URL could serve different bytes.  Update both constants together,
# together with the "Stockfish 17.1" version check at the end.
set -euo pipefail

VER="sf_17.1"
ASSET="stockfish-ubuntu-x86-64-avx2.tar"
SHA256="09e953222dbe80aaea5c33dab265413b295cb8376418445c877708c61e31987d"
DEST="${1:-$HOME/.local/bin/stockfish}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

curl -fL -o "$tmp/sf.tar" \
  "https://github.com/official-stockfish/Stockfish/releases/download/${VER}/${ASSET}"

echo "${SHA256}  ${tmp}/sf.tar" | sha256sum -c -

tar -xf "$tmp/sf.tar" -C "$tmp"
install -m755 "$tmp/stockfish/${ASSET%.tar}" "$DEST"

name="$("$DEST" <<< $'uci\nquit' | sed -n 's/^id name //p' | head -n 1)"
case "$name" in
  "Stockfish 17.1"*) echo "installed $DEST ($name)" ;;
  *) echo "refusing: $DEST reports '$name', expected Stockfish 17.1" >&2; exit 1 ;;
esac
