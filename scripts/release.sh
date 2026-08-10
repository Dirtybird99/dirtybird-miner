#!/bin/bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <version> [build-dir] [output-dir]" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${1#v}"
BUILD_DIR="${2:-build}"
OUTPUT_DIR="${3:-dist}"

if [[ -z "$VERSION" ]]; then
    echo "Version cannot be empty." >&2
    exit 1
fi

ASSET_VERSION="v$VERSION"
BINARY_NAME="dirtybird-c-miner"
BUILD_ROOT="$REPO_ROOT/$BUILD_DIR"
BIN_DIR="$BUILD_ROOT/bin"
BINARY_PATH="$BIN_DIR/$BINARY_NAME"
STAGE_ROOT="$REPO_ROOT/$OUTPUT_DIR"
PACKAGE_ARCH="${PACKAGE_ARCH:-amd64}"
case "$PACKAGE_ARCH" in
    amd64) CPU_NOTE="64-bit AVX2 CPUs." ;;
    arm64) CPU_NOTE="64-bit ARMv8 (aarch64) glibc Linux." ;;
    *) echo "Unsupported PACKAGE_ARCH: $PACKAGE_ARCH" >&2; exit 1 ;;
esac
PACKAGE_NAME="dirtybird-c-miner-$PACKAGE_ARCH-$ASSET_VERSION"
PACKAGE_DIR="$STAGE_ROOT/$PACKAGE_NAME"
ARCHIVE_PATH="$STAGE_ROOT/$PACKAGE_NAME.tar.gz"
LIB_DIR="$PACKAGE_DIR/lib"

echo "================================================"
echo "DIRTYBIRD C Miner Linux Packaging"
echo "Version: $ASSET_VERSION"
echo "================================================"

if [[ ! -f "$BINARY_PATH" ]]; then
    echo "Binary not found: $BINARY_PATH" >&2
    exit 1
fi

for tool in ldd patchelf readelf; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool is required for Linux packaging." >&2; exit 1; }
done

# Bundle every non-core shared library the binary needs (libssl/libcrypto/libstdc++/...).
should_bundle_runtime() {
    case "$1" in
        linux-vdso.so.1|linux-gate.so.1|ld-linux*.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libutil.so.*|libresolv.so.*|libnsl.so.*|libanl.so.*)
            return 1 ;;
        *) return 0 ;;
    esac
}

bundle_runtime_libs() {
    local binary_path="$1" output_dir="$2" line soname actual_path resolved_path
    local -a missing_libs=()
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+not[[:space:]]+found$ ]]; then
            soname="$(basename "${BASH_REMATCH[1]}")"
            should_bundle_runtime "$soname" && missing_libs+=("$soname")
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+([^[:space:]]+)[[:space:]]+\(0x[0-9a-fA-F]+\)$ ]]; then
            soname="$(basename "${BASH_REMATCH[1]}")"; actual_path="${BASH_REMATCH[2]}"
            should_bundle_runtime "$soname" || continue
            resolved_path="$(readlink -f "$actual_path")"
            install -m 0644 "$resolved_path" "$output_dir/$soname"
        fi
    done < <(ldd "$binary_path")
    if [[ ${#missing_libs[@]} -gt 0 ]]; then
        printf 'Missing runtime libraries:%s\n' " ${missing_libs[*]}" >&2
        exit 1
    fi
}

validate_runtime_bundle() {
    local binary_path="$1" output_dir="$2" soname actual_rpath
    while IFS= read -r soname; do
        if should_bundle_runtime "$soname" && [[ ! -f "$output_dir/$soname" ]]; then
            echo "Bundled dependency missing from package: $soname" >&2
            exit 1
        fi
    done < <(readelf -d "$binary_path" | sed -n 's/^.*Shared library: \[\(.*\)\]$/\1/p')
    actual_rpath="$(patchelf --print-rpath "$binary_path")"
    if [[ "$actual_rpath" != '$ORIGIN/lib' ]]; then
        echo "Unexpected runtime search path: $actual_rpath" >&2
        exit 1
    fi
}

mkdir -p "$STAGE_ROOT"
rm -rf "$PACKAGE_DIR"
mkdir -p "$LIB_DIR"

cp "$BINARY_PATH" "$PACKAGE_DIR/"
chmod +x "$PACKAGE_DIR/$BINARY_NAME"
bundle_runtime_libs "$PACKAGE_DIR/$BINARY_NAME" "$LIB_DIR"
patchelf --set-rpath '$ORIGIN/lib' "$PACKAGE_DIR/$BINARY_NAME"
validate_runtime_bundle "$PACKAGE_DIR/$BINARY_NAME" "$LIB_DIR"

cp "$REPO_ROOT/README.md" "$PACKAGE_DIR/"
cp "$REPO_ROOT/LICENSE" "$PACKAGE_DIR/"
cp "$REPO_ROOT/config.json" "$PACKAGE_DIR/"
cp "$REPO_ROOT/config.json.example" "$PACKAGE_DIR/"

cat > "$PACKAGE_DIR/start.sh" <<'EOF'
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$SCRIPT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$SCRIPT_DIR"
exec ./dirtybird-c-miner "$@"
EOF
chmod +x "$PACKAGE_DIR/start.sh"

cat > "$PACKAGE_DIR/QUICKSTART.txt" <<EOF
DIRTYBIRD C Miner $ASSET_VERSION
==============================

Contents:
- dirtybird-c-miner
- lib/          (bundled runtime libraries)
- config.json   (edit this: daemon-address / wallet / threads / priority)
- config.json.example
- start.sh
- README.md
- LICENSE

Quick start:
1. Edit config.json: "daemon-address" (host:port), "wallet", "threads" (-1 = auto), "priority".
2. ./start.sh        (reads config.json)
   Override per-run by appending flags (CLI wins over config.json):
   ./start.sh -t 20 -p max

Use -p max for headless/AFK (more hashrate); -p normal (default) is desktop-safe.

Notes:
- $CPU_NOTE Non-core runtime libraries are bundled under ./lib.
- Startup prints a pow("a") self-test; it must say PASS.
EOF

rm -f "$ARCHIVE_PATH"
tar -czf "$ARCHIVE_PATH" -C "$STAGE_ROOT" "$PACKAGE_NAME"

echo "Created package: $ARCHIVE_PATH"
