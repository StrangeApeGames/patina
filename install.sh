#!/bin/sh
# Installs the latest Patina release (the binary, plus the skill and MCP server for Claude Code and Codex):
#   curl -fsSL https://raw.githubusercontent.com/StrangeApeGames/patina/main/install.sh | sh
# Afterwards `patina update` keeps it current; its MCP server tells your agent when to run it.
set -eu
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) asset=patina-macos-arm64 ;;
  *) echo "no prebuilt Patina for $(uname -s) $(uname -m); build from source (see README)" >&2; exit 1 ;;
esac
base=https://github.com/StrangeApeGames/patina/releases/latest/download
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$asset" "$base/$asset"
curl -fsSL -o "$tmp/SHA256SUMS.txt" "$base/SHA256SUMS.txt"
(cd "$tmp" && grep " $asset\$" SHA256SUMS.txt | shasum -a 256 -c - >/dev/null) || { echo "checksum mismatch for $asset" >&2; exit 1; }
chmod +x "$tmp/$asset"
"$tmp/$asset" install "$@"
