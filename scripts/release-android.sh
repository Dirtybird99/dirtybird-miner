#!/bin/bash
# Package the NDK-built aarch64 Android (Bionic) binary.
# Expects a standalone binary (ANDROID_STL=c++_static, static OpenSSL) at
# <build-dir>/bin/dirtybird-c-miner. Layout: <pkg>/{dirtybird-c-miner,
# README.md, LICENSE, QUICKSTART.txt}.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <version> [build-dir] [output-dir]" >&2; exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${1#v}"
BUILD_DIR="${2:-build-android}"
OUTPUT_DIR="${3:-dist}"
[[ -z "$VERSION" ]] && { echo "Version cannot be empty." >&2; exit 1; }

ASSET_VERSION="v$VERSION"
BINARY_NAME="dirtybird-c-miner"
BINARY_PATH="$REPO_ROOT/$BUILD_DIR/bin/$BINARY_NAME"
STAGE_ROOT="$REPO_ROOT/$OUTPUT_DIR"
PACKAGE_NAME="dirtybird-c-miner-${ASSET_VERSION}_aarch64_android"
PACKAGE_DIR="$STAGE_ROOT/$PACKAGE_NAME"
ARCHIVE_PATH="$STAGE_ROOT/$PACKAGE_NAME.tar.gz"

echo "=== DIRTYBIRD Android (aarch64) Packaging $ASSET_VERSION ==="
[[ -f "$BINARY_PATH" ]] || { echo "Binary not found: $BINARY_PATH" >&2; exit 1; }

mkdir -p "$PACKAGE_DIR"
cp "$BINARY_PATH" "$PACKAGE_DIR/"
chmod +x "$PACKAGE_DIR/$BINARY_NAME"
[[ -f "$REPO_ROOT/README.md" ]] && cp "$REPO_ROOT/README.md" "$PACKAGE_DIR/"
[[ -f "$REPO_ROOT/LICENSE" ]]   && cp "$REPO_ROOT/LICENSE"   "$PACKAGE_DIR/"

cat > "$PACKAGE_DIR/QUICKSTART.txt" <<EOF
DIRTYBIRD C Miner $ASSET_VERSION (aarch64 / Android)
==================================================

Contents:
- dirtybird-c-miner   (standalone aarch64 binary; static libc++ + OpenSSL)
- README.md, LICENSE

Run (Termux or adb shell):
  chmod +x ./dirtybird-c-miner
  ./dirtybird-c-miner -d <daemon host:port> -w <your DERO wallet> -t <threads>

On Android, -p max is currently a no-op; priority tuning is Windows-only.
Use -t <threads> to balance hashrate, temperature, and battery use.
Startup prints a pow("a") self-test; it must say PASS.
EOF

rm -f "$ARCHIVE_PATH"
tar -czf "$ARCHIVE_PATH" -C "$STAGE_ROOT" "$PACKAGE_NAME"
echo "Created package: $ARCHIVE_PATH"
