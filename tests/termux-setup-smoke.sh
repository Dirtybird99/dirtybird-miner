#!/usr/bin/env bash
set -euo pipefail

command -v jq >/dev/null || {
    echo "jq is required for this test" >&2
    exit 1
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TEST_DIR"' EXIT

FAKE_BIN="$TEST_DIR/bin"
LEGACY_INSTALL_DIR="$TEST_DIR/home/dirtybird-miner"
INSTALL_DIR="$TEST_DIR/home/dirtybird-c-miner"
mkdir -p "$FAKE_BIN" "$LEGACY_INSTALL_DIR"

cat > "$FAKE_BIN/uname" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
    -m) printf 'aarch64\n' ;;
    -o) printf 'Android\n' ;;
    *)  printf 'Linux\n' ;;
esac
EOF

cat > "$FAKE_BIN/curl" <<'EOF'
#!/usr/bin/env bash
if [ "${TEST_CURL_FAIL:-0}" = 1 ]; then
    exit 22
fi

url=""
output=""
previous=""
for arg in "$@"; do
    if [ "$previous" = "-o" ]; then
        output="$arg"
    fi
    case "$arg" in
        http*) url="$arg" ;;
    esac
    previous="$arg"
    if [ "$arg" = "https://api.github.com/repos/Dirtybird99/Dirtybird-C-Miner/releases/latest" ]; then
        printf '{"tag_name":"%s"}\n' "${TEST_LATEST_TAG:-v1.0.30}"
        exit 0
    fi
done

if [ -n "${TEST_CURL_LOG:-}" ]; then
    printf '%s\n' "$url" >> "$TEST_CURL_LOG"
fi
if [ -n "${TEST_LEGACY_ARCHIVE:-}" ] &&
   [[ "$url" == *dirtybird-c-miner-v* ]]; then
    exit 22
fi
if [ -n "${TEST_LEGACY_ARCHIVE:-}" ] &&
   [[ "$url" == *dirtybird-miner-v* ]]; then
    cp "$TEST_LEGACY_ARCHIVE" "$output"
    exit 0
fi
echo "unexpected curl request: $*" >&2
exit 99
EOF

for command_name in wget pkg; do
    cat > "$FAKE_BIN/$command_name" <<'EOF'
#!/usr/bin/env bash
echo "unexpected command: ${0##*/} $*" >&2
exit 99
EOF
done

# TEST_TERMUX_HANG reproduces the real-world case the timeout guards exist for:
# the termux-api PACKAGE installed (so `command -v` succeeds) but the companion
# APP absent, where every termux-* call blocks forever.
cat > "$FAKE_BIN/termux-battery-status" <<'EOF'
#!/usr/bin/env bash
if [ "${TEST_TERMUX_HANG:-0}" = 1 ]; then
    sleep 300
fi
printf '{"percentage":80,"plugged":"%s"}\n' "${TEST_PLUGGED:-PLUGGED_USB}"
EOF

cat > "$FAKE_BIN/termux-wake-lock" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF

cat > "$FAKE_BIN/termux-wake-unlock" <<'EOF'
#!/usr/bin/env bash
touch "$TEST_UNLOCK_MARKER"
EOF

cat > "$LEGACY_INSTALL_DIR/dirtybird-miner-cpu" <<'EOF'
#!/usr/bin/env bash
if [ "$#" -eq 1 ] && [ "$1" = "--version" ]; then
    printf '%s\n' "--version" >> "$TEST_INVOCATION_LOG"
    printf 'Dirtybird Miner v1.0.29\n'
elif [ "$#" -eq 0 ]; then
    printf '%s\n' "<run>" >> "$TEST_INVOCATION_LOG"
else
    printf 'unexpected miner arguments: %s\n' "$*" >&2
    exit 64
fi
EOF

cat > "$LEGACY_INSTALL_DIR/config.json" <<'EOF'
{
  "daemon-address": "community-pools.mysrv.cloud:10300",
  "wallet": "dero1test",
  "threads": 7,
  "priority": "normal"
}
EOF

printf 'v1.0.29\n' > "$LEGACY_INSTALL_DIR/.installed_version"
chmod +x "$FAKE_BIN"/* "$LEGACY_INSTALL_DIR/dirtybird-miner-cpu"

export TEST_INVOCATION_LOG="$TEST_DIR/invocations.log"
export TEST_UNLOCK_MARKER="$TEST_DIR/unlocked"

assert_contains() {
    if ! grep -Fq -- "$2" <<<"$1"; then
        printf 'missing output: %s\n%s\n' "$2" "$1" >&2
        exit 1
    fi
}

assert_not_contains() {
    if grep -Fq -- "$2" <<<"$1"; then
        printf 'unexpected output: %s\n%s\n' "$2" "$1" >&2
        exit 1
    fi
}

OUTPUT="$(
    HOME="$TEST_DIR/home" PATH="$FAKE_BIN:$PATH" \
        bash "$ROOT/scripts/termux-setup.sh" 2>&1
)"

assert_contains "$OUTPUT" "Migrating legacy installation from $LEGACY_INSTALL_DIR"
assert_contains "$OUTPUT" "Update available: Dirtybird Miner v1.0.29 -> Dirtybird C Miner v1.0.30."
assert_contains "$OUTPUT" "Update before benchmarking:"
assert_contains "$OUTPUT" "curl -fsSL https://raw.githubusercontent.com/Dirtybird99/Dirtybird-C-Miner/master/scripts/termux-setup.sh | bash -s -- --update"
PLAIN_OUTPUT="$(printf '%s\n' "$OUTPUT" | sed $'s/\033\\[[0-9;]*m//g')"
assert_contains "$PLAIN_OUTPUT" "  Version:  Dirtybird Miner v1.0.29"
assert_contains "$OUTPUT" "Could not acquire wake-lock. Android Doze may pause the miner in background."
assert_not_contains "$OUTPUT" "running on battery power"
assert_not_contains "$OUTPUT" "Thermal throttling"

if [ -e "$LEGACY_INSTALL_DIR" ] || [ ! -x "$INSTALL_DIR/dirtybird-c-miner" ]; then
    echo "legacy installation was not migrated and renamed" >&2
    exit 1
fi
if [ "$(jq -r '.threads' "$INSTALL_DIR/config.json")" != 7 ] ||
   [ "$(cat "$INSTALL_DIR/.installed_version")" != v1.0.29 ]; then
    echo "legacy configuration or release marker was not preserved" >&2
    exit 1
fi

OFFLINE_OUTPUT="$(
    HOME="$TEST_DIR/home" PATH="$FAKE_BIN:$PATH" TEST_CURL_FAIL=1 TEST_PLUGGED=UNPLUGGED \
        bash "$ROOT/scripts/termux-setup.sh" 2>&1
)"

assert_contains "$OFFLINE_OUTPUT" "Device is running on battery power. Mining drains battery fast."
assert_not_contains "$OFFLINE_OUTPUT" "Could not determine latest release"

EXPECTED_INVOCATIONS=$'--version\n<run>\n--version\n<run>'
if [ "$(cat "$TEST_INVOCATION_LOG")" != "$EXPECTED_INVOCATIONS" ]; then
    printf 'unexpected miner invocations:\n%s\n' "$(cat "$TEST_INVOCATION_LOG")" >&2
    exit 1
fi

if [ -e "$TEST_UNLOCK_MARKER" ]; then
    echo "wake unlock ran even though wake lock acquisition failed" >&2
    exit 1
fi

# A fresh install while v1.0.36 is latest must fall back to the legacy-named
# Android asset, normalize its binary name, and accept its legacy version text.
FALLBACK_HOME="$TEST_DIR/fallback-home"
FALLBACK_INSTALL_DIR="$FALLBACK_HOME/dirtybird-c-miner"
ARCHIVE_STAGE="$TEST_DIR/archive-stage/dirtybird-miner-v1.0.36_aarch64_android"
mkdir -p "$FALLBACK_INSTALL_DIR" "$ARCHIVE_STAGE"
cat > "$FALLBACK_INSTALL_DIR/config.json" <<'EOF'
{"daemon-address":"community-pools.mysrv.cloud:10300","wallet":"dero1test","threads":7,"priority":"normal"}
EOF
cat > "$ARCHIVE_STAGE/dirtybird-miner-cpu" <<'EOF'
#!/usr/bin/env bash
if [ "$#" -eq 1 ] && [ "$1" = "--version" ]; then
    printf 'Dirtybird Miner v1.0.36\n'
elif [ "$#" -eq 0 ]; then
    printf '<run>\n' >> "$TEST_FALLBACK_INVOCATION_LOG"
else
    exit 64
fi
EOF
chmod +x "$ARCHIVE_STAGE/dirtybird-miner-cpu"
LEGACY_ARCHIVE="$TEST_DIR/dirtybird-miner-v1.0.36_aarch64_android.tar.gz"
tar -czf "$LEGACY_ARCHIVE" -C "$TEST_DIR/archive-stage" "${ARCHIVE_STAGE##*/}"
DOWNLOAD_LOG="$TEST_DIR/downloads.log"
export TEST_FALLBACK_INVOCATION_LOG="$TEST_DIR/fallback-invocations.log"

FALLBACK_OUTPUT="$(
    HOME="$FALLBACK_HOME" PATH="$FAKE_BIN:$PATH" \
        TEST_LATEST_TAG=v1.0.36 TEST_LEGACY_ARCHIVE="$LEGACY_ARCHIVE" \
        TEST_CURL_LOG="$DOWNLOAD_LOG" bash "$ROOT/scripts/termux-setup.sh" 2>&1
)"

assert_contains "$FALLBACK_OUTPUT" "trying the legacy release asset"
assert_not_contains "$FALLBACK_OUTPUT" "Update available:"
if [ ! -x "$FALLBACK_INSTALL_DIR/dirtybird-c-miner" ] ||
   [ -e "$FALLBACK_INSTALL_DIR/dirtybird-miner-cpu" ] ||
   [ "$(cat "$FALLBACK_INSTALL_DIR/.installed_version")" != v1.0.36 ]; then
    echo "legacy release asset was not normalized after extraction" >&2
    exit 1
fi
EXPECTED_DOWNLOADS=$'https://github.com/Dirtybird99/Dirtybird-C-Miner/releases/download/v1.0.36/dirtybird-c-miner-v1.0.36_aarch64_android.tar.gz\nhttps://github.com/Dirtybird99/Dirtybird-C-Miner/releases/download/v1.0.36/dirtybird-miner-v1.0.36_aarch64_android.tar.gz'
if [ "$(cat "$DOWNLOAD_LOG")" != "$EXPECTED_DOWNLOADS" ]; then
    printf 'unexpected Android asset fallback order:\n%s\n' "$(cat "$DOWNLOAD_LOG")" >&2
    exit 1
fi
if [ "$(cat "$TEST_FALLBACK_INVOCATION_LOG")" != "<run>" ]; then
    echo "fallback miner did not run" >&2
    exit 1
fi

# Uninstall cleans up both names so a pre-rebrand install is not orphaned.
mkdir -p "$FALLBACK_HOME/dirtybird-miner"
HOME="$FALLBACK_HOME" PATH="$FAKE_BIN:$PATH" \
    bash "$ROOT/scripts/termux-setup.sh" --uninstall >/dev/null 2>&1
if [ -e "$FALLBACK_INSTALL_DIR" ] || [ -e "$FALLBACK_HOME/dirtybird-miner" ]; then
    echo "uninstall left a current or legacy installation behind" >&2
    exit 1
fi

# A hung termux-api must not stall the installer. Without the `timeout` guards
# this call blocks before mining ever starts, so the phone sits idle with no
# output and no error -- 20s here is ~4x the 5s guard and far below the stub's
# 300s sleep, so it cannot pass by accident.
set +e
timeout 20 env HOME="$TEST_DIR/home" PATH="$FAKE_BIN:$PATH" \
    TEST_TERMUX_HANG=1 bash "$ROOT/scripts/termux-setup.sh" >/dev/null 2>&1
HANG_RC=$?
set -e
if [ "$HANG_RC" -eq 124 ]; then
    echo "installer hung on a blocked termux-api call (missing timeout guard)" >&2
    exit 1
fi

# The wallet regex is the ONLY guard against a truncated paste: the miner itself
# checks only that the address is non-empty, so a short wallet mines to nobody
# and is discovered hours later.
if ! grep -q 'a-z0-9\]{60,}' "$ROOT/scripts/termux-setup.sh"; then
    echo "wallet validation lost its length floor" >&2
    exit 1
fi
if printf 'dero1abc' | grep -qE '^(dero1|deto1)[a-z0-9]{60,}$'; then
    echo "truncated wallet accepted by the validation pattern" >&2
    exit 1
fi
if ! printf 'dero1qyvuemd6z0uzsx5ufc99f0jhyzvvpysmrd2t3526ht7a9dfh7jve2qqt0vu5y' \
        | grep -qE '^(dero1|deto1)[a-z0-9]{60,}$'; then
    echo "a real wallet address is rejected by the validation pattern" >&2
    exit 1
fi

# The documented one-liner pipes straight into bash, so it must fail closed:
# without -f, GitHub's 404/rate-limit HTML body is handed to the shell.
if grep -qE 'curl -[a-zA-Z]*sL[a-zA-Z]* .*termux-setup\.sh' "$ROOT/README.md"; then
    echo "README pipes an unchecked curl into bash -- use curl -fsSL" >&2
    exit 1
fi

echo "termux setup smoke: PASS"
