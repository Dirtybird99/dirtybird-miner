#!/bin/bash
# Package the Apple Silicon macOS binary.
# Expects a binary with OpenSSL statically linked (OPENSSL_USE_STATIC_LIBS) at
# <build-dir>/bin/dirtybird-c-miner, so the package needs no bundled dylibs.
# Layout: <pkg>/{dirtybird-c-miner, config.json, config.json.example,
# start.sh, README.md, LICENSE, QUICKSTART.txt}.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <version> [build-dir] [output-dir]" >&2; exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${1#v}"
BUILD_DIR="${2:-build}"
OUTPUT_DIR="${3:-dist}"
[[ -z "$VERSION" ]] && { echo "Version cannot be empty." >&2; exit 1; }

ASSET_VERSION="v$VERSION"
BINARY_NAME="dirtybird-c-miner"
BINARY_PATH="$REPO_ROOT/$BUILD_DIR/bin/$BINARY_NAME"
STAGE_ROOT="$REPO_ROOT/$OUTPUT_DIR"
PACKAGE_NAME="dirtybird-c-miner-macos-arm64-$ASSET_VERSION"
PACKAGE_DIR="$STAGE_ROOT/$PACKAGE_NAME"
ARCHIVE_PATH="$STAGE_ROOT/$PACKAGE_NAME.tar.gz"

echo "=== DIRTYBIRD macOS (arm64) Packaging $ASSET_VERSION ==="
[[ -f "$BINARY_PATH" ]] || { echo "Binary not found: $BINARY_PATH" >&2; exit 1; }
command -v otool >/dev/null 2>&1 || { echo "otool is required for macOS packaging." >&2; exit 1; }

# The package ships no dylibs, so OpenSSL must be linked statically; a dynamic
# libssl/libcrypto reference would break on any machine without Homebrew.
if otool -L "$BINARY_PATH" | grep -E 'libssl|libcrypto'; then
    echo "ERROR: dynamic OpenSSL linkage detected; build with OPENSSL_USE_STATIC_LIBS=TRUE." >&2
    exit 1
fi

mkdir -p "$STAGE_ROOT"
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR"
cp "$BINARY_PATH" "$PACKAGE_DIR/"
chmod +x "$PACKAGE_DIR/$BINARY_NAME"
cp "$REPO_ROOT/README.md" "$PACKAGE_DIR/"
cp "$REPO_ROOT/LICENSE" "$PACKAGE_DIR/"
cp "$REPO_ROOT/config.json" "$PACKAGE_DIR/"
cp "$REPO_ROOT/config.json.example" "$PACKAGE_DIR/"

cat > "$PACKAGE_DIR/start.sh" <<'EOF'
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
exec ./dirtybird-c-miner "$@"
EOF
chmod +x "$PACKAGE_DIR/start.sh"

cat > "$PACKAGE_DIR/QUICKSTART.txt" <<EOF
DIRTYBIRD C Miner $ASSET_VERSION (macOS / Apple Silicon)
======================================================

Contents:
- dirtybird-c-miner   (arm64 binary; OpenSSL statically linked)
- config.json           (edit this: daemon-address / wallet / threads / priority)
- config.json.example
- start.sh
- README.md
- LICENSE

Quick start:
1. Edit config.json: "daemon-address" (host:port), "wallet", "threads" (-1 = auto).
2. ./start.sh        (reads config.json)
   Override per-run by appending flags (CLI wins over config.json):
   ./start.sh -t 8

First run: the binary is not signed or notarized, so Gatekeeper may block it.
Either right-click > Open once, or clear the quarantine flag:
  xattr -d com.apple.quarantine ./dirtybird-c-miner

On macOS, -p max is currently a no-op; priority tuning is Windows-only.
Startup prints a pow("a") self-test; it must say PASS.
EOF

rm -f "$ARCHIVE_PATH"
tar -czf "$ARCHIVE_PATH" -C "$STAGE_ROOT" "$PACKAGE_NAME"
echo "Created package: $ARCHIVE_PATH"
