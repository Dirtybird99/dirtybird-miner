#!/bin/bash
# Package the Linux amd64 binary as a HiveOS / MMPOS custom-miner archive.
# Layout: <pkg>/{dirtybird-c-miner, lib/, h-manifest.conf, h-config.sh, h-run.sh, h-stats.sh}.
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
BIN_DIR="$REPO_ROOT/$BUILD_DIR/bin"
BINARY_PATH="$BIN_DIR/$BINARY_NAME"
STAGE_ROOT="$REPO_ROOT/$OUTPUT_DIR"
PACKAGE_NAME="dirtybird-c-miner-${ASSET_VERSION}_linux_hiveos_mmpos"
PACKAGE_DIR="$STAGE_ROOT/$BINARY_NAME"
ARCHIVE_PATH="$STAGE_ROOT/$PACKAGE_NAME.tar.gz"
LIB_DIR="$PACKAGE_DIR/lib"

echo "=== DIRTYBIRD HiveOS/MMPOS Packaging $ASSET_VERSION ==="
[[ -f "$BINARY_PATH" ]] || { echo "Binary not found: $BINARY_PATH" >&2; exit 1; }
for tool in ldd patchelf readelf; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool required." >&2; exit 1; }
done

should_bundle() {
    case "$1" in
        linux-vdso.so.1|linux-gate.so.1|ld-linux*.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libutil.so.*|libresolv.so.*|libnsl.so.*|libanl.so.*) return 1 ;;
        *) return 0 ;;
    esac
}

rm -rf "$PACKAGE_DIR"
mkdir -p "$LIB_DIR"
cp "$BINARY_PATH" "$PACKAGE_DIR/"; chmod +x "$PACKAGE_DIR/$BINARY_NAME"

# bundle non-core shared libs and set rpath to ./lib
while IFS= read -r line; do
    if [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+([^[:space:]]+)[[:space:]]+\(0x[0-9a-fA-F]+\)$ ]]; then
        so="$(basename "${BASH_REMATCH[1]}")"; p="${BASH_REMATCH[2]}"
        should_bundle "$so" || continue
        install -m 0644 "$(readlink -f "$p")" "$LIB_DIR/$so"
    fi
done < <(ldd "$PACKAGE_DIR/$BINARY_NAME")
patchelf --set-rpath '$ORIGIN/lib' "$PACKAGE_DIR/$BINARY_NAME"

# HiveOS/MMPOS integration files
cp "$REPO_ROOT/config/h-manifest.conf" "$PACKAGE_DIR/"
cp "$REPO_ROOT/config/h-config.sh"     "$PACKAGE_DIR/"
cp "$REPO_ROOT/config/h-run.sh"        "$PACKAGE_DIR/"
cp "$REPO_ROOT/config/h-stats.sh"      "$PACKAGE_DIR/"
chmod +x "$PACKAGE_DIR"/h-*.sh
[[ -f "$REPO_ROOT/README.md" ]] && cp "$REPO_ROOT/README.md" "$PACKAGE_DIR/"
[[ -f "$REPO_ROOT/LICENSE" ]]   && cp "$REPO_ROOT/LICENSE"   "$PACKAGE_DIR/"

rm -f "$ARCHIVE_PATH"
tar -czf "$ARCHIVE_PATH" -C "$STAGE_ROOT" "$BINARY_NAME"
echo "Created package: $ARCHIVE_PATH"
