#!/usr/bin/env bash
#
# DIRTYBIRD C Miner -- Termux (Android) setup & launcher.
#
# Download-only installer: fetches the pre-built aarch64 Android release from
# GitHub, writes config.json, acquires a wake-lock so Android Doze doesn't kill
# the miner, and runs it with auto-restart.
#
# Usage:
#   bash scripts/termux-setup.sh             # install (if needed) + run
#   bash scripts/termux-setup.sh --update     # force re-download latest release
#   bash scripts/termux-setup.sh --reconfigure  # re-prompt for pool/wallet/threads
#   bash scripts/termux-setup.sh --uninstall  # remove installed files
#   bash scripts/termux-setup.sh --help
#
set -euo pipefail

REPO="Dirtybird99/Dirtybird-C-Miner"
DEFAULT_WALLET="dero1qyvuemd6z0uzsx5ufc99f0jhyzvvpysmrd2t3526ht7a9dfh7jve2qqt0vu5y"
INSTALL_DIR="$HOME/dirtybird-c-miner"
BINARY_NAME="dirtybird-c-miner"
VERSION_FILE=".installed_version"
ARCHIVE_PREFIX="dirtybird-c-miner-v"
ARCHIVE_SUFFIX="_aarch64_android.tar.gz"
LEGACY_INSTALL_DIR="$HOME/dirtybird-miner"
LEGACY_BINARY_NAME="dirtybird-miner-cpu"
LEGACY_ARCHIVE_PREFIX="dirtybird-miner-v"

# ── daemon / pool menu ────────────────────────────────────────────────────────
# The two pools hand out low-difficulty shares, so a phone sees progress every
# few seconds; the solo daemons hand out real network work, which is the same
# expected earnings for a given hashrate but can leave a phone sitting for hours
# between rewards -- and that reads as a broken miner. Hence the labels.
#
# Port matters as much as host: dero.rabidmining.com serves solo daemon work on
# :10100 and pool shares on :10300 (network difficulty vs 20000 when measured).
# The pool port is the one worth offering here.
#
# No scheme prefix: the miner splits the address at the last ':' (load_config in
# src/main.cpp), so a "ws://" prefix becomes part of the hostname and the lookup
# fails.
#
# Verified 2026-07-24 -- all four completed a TLS handshake and a
# 'GET /ws/<wallet>' WebSocket upgrade and served a job. A plain TCP probe is
# not enough to re-check them: the miner only ever speaks TLS, so a port that
# accepts a connection can still fail at SSL_connect. To re-check one:
#   KEY=$(head -c16 /dev/urandom | base64)
#   printf 'GET /ws/WALLET HTTP/1.1\r\nHost: HOST:PORT\r\nUpgrade: websocket\r\n'\
#          'Connection: Upgrade\r\nSec-WebSocket-Key: '"$KEY"'\r\n'\
#          'Sec-WebSocket-Version: 13\r\n\r\n' \
#     | openssl s_client -connect HOST:PORT -quiet   # expect "101 Switching Protocols"
declare -a DAEMON_NAMES=(
    "Community Pools"
    "Rabid Mining"
    "dero-node.net"
    "DERO Foundation"
    "Custom address"
)
declare -a DAEMON_ADDRS=(
    "community-pools.mysrv.cloud:10300"
    "dero.rabidmining.com:10300"
    "dero-node.net:10100"
    "node.derofoundation.org:10100"
    ""
)
declare -a DAEMON_KINDS=(
    "pool -- rewards every few seconds, best for phones"
    "pool -- rewards every few seconds, best for phones"
    "solo node -- a phone may wait hours between rewards"
    "solo node -- full blocks only, 9x the work per reward"
    ""
)

# ── colours (safe for Termux) ──────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

info()  { printf "${GREEN}[*]${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}[!]${NC} %s\n" "$*"; }
err()   { printf "${RED}[x]${NC} %s\n" "$*" >&2; }
note()  { printf "${CYAN}[i]${NC} %s\n" "$*"; }

# ── flags ─────────────────────────────────────────────────────────────────────
FORCE_UPDATE=false
RECONFIGURE=false
UNINSTALL=false

usage() {
    cat <<'USAGE'
DIRTYBIRD C Miner -- Termux (Android) setup & launcher.

Downloads the pre-built aarch64 Android release, writes config.json, acquires a
wake-lock so Android Doze doesn't pause mining, and runs with auto-restart.

Usage:
  bash scripts/termux-setup.sh                # install (if needed) + run
  bash scripts/termux-setup.sh --update       # re-download the latest release
  bash scripts/termux-setup.sh --reconfigure  # re-prompt for pool/wallet/threads
  bash scripts/termux-setup.sh --uninstall    # remove installed files
  bash scripts/termux-setup.sh --help         # this message

Requires aarch64 (64-bit ARM) Android. Install termux-api ("pkg install
termux-api") for wake-lock and battery-status support.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --update)        FORCE_UPDATE=true; shift ;;
        --reconfigure)   RECONFIGURE=true; shift ;;
        --uninstall)     UNINSTALL=true; shift ;;
        -h|--help)
            usage
            exit 0
            ;;
        *) err "Unknown option: $1"; exit 2 ;;
    esac
done

# ── detect platform ───────────────────────────────────────────────────────────
IS_ANDROID=false
ARCH="$(uname -m)"
if [ "$(uname -o 2>/dev/null)" = "Android" ]; then
    IS_ANDROID=true
fi

if [ "$IS_ANDROID" = true ]; then
    if [ "$ARCH" != "aarch64" ]; then
        err "Android on $ARCH is not supported by this script."
        err "Only aarch64 (64-bit ARM) Android is supported."
        err "For other platforms, download the appropriate release manually:"
        err "  https://github.com/$REPO/releases"
        exit 1
    fi
else
    err "This script is for Android/Termux only."
    err "On Linux x86_64, use the amd64 release:"
    err "  https://github.com/$REPO/releases  (dirtybird-c-miner-amd64-v*.tar.gz)"
    exit 1
fi

# ── step 1: install deps ──────────────────────────────────────────────────────
info "Checking dependencies..."
need_install=()
for cmd in tar; do
    command -v "$cmd" &>/dev/null || need_install+=("$cmd")
done
# prefer curl, fall back to wget
if ! command -v curl &>/dev/null && ! command -v wget &>/dev/null; then
    need_install+=(curl)
fi
if ! command -v jq &>/dev/null; then
    need_install+=(jq)
fi

if [ "${#need_install[@]}" -gt 0 ]; then
    info "Installing: ${need_install[*]}"
    pkg update -y >/dev/null 2>&1 || true
    pkg install -y "${need_install[@]}" >/dev/null 2>&1 || {
        err "Failed to install: ${need_install[*]}"
        err "Run: pkg install -y ${need_install[*]}"
        exit 1
    }
fi
info "Dependencies OK."

# ── step 2: handle --uninstall ────────────────────────────────────────────────
if [ "$UNINSTALL" = true ]; then
    info "Removing Dirtybird C Miner installations ..."
    rm -rf -- "$INSTALL_DIR" "$LEGACY_INSTALL_DIR"
    info "Done. (Config and binaries removed.)"
    exit 0
fi

# ── step 3: get the binary ────────────────────────────────────────────────────
# Releases before the rebrand installed here. Move the complete directory so
# config.json and the installed-version marker survive the upgrade.
if [ ! -e "$INSTALL_DIR" ] && [ -d "$LEGACY_INSTALL_DIR" ]; then
    info "Migrating legacy installation from $LEGACY_INSTALL_DIR ..."
    mv "$LEGACY_INSTALL_DIR" "$INSTALL_DIR"
    if [ ! -e "$INSTALL_DIR/$BINARY_NAME" ] &&
       [ -e "$INSTALL_DIR/$LEGACY_BINARY_NAME" ]; then
        mv "$INSTALL_DIR/$LEGACY_BINARY_NAME" "$INSTALL_DIR/$BINARY_NAME"
    fi
elif [ -e "$INSTALL_DIR" ] && [ -d "$LEGACY_INSTALL_DIR" ]; then
    warn "Both current and legacy installations exist; using $INSTALL_DIR."
fi

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# HTTP fetcher for API calls (stdout): curl preferred, wget fallback
fetch() {
    if command -v curl &>/dev/null; then
        curl -fsSL --connect-timeout 5 --max-time 10 "$1"
    else
        wget -qO- -T 10 "$1"
    fi
}

latest_release_tag() {
    fetch "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
        | jq -r '.tag_name // empty' 2>/dev/null || true
}

download_file() {
    local url="$1" output="$2"
    rm -f "$output"
    if command -v curl &>/dev/null; then
        curl -fsSL "$url" -o "$output" && return 0
    else
        wget -q --show-progress -O "$output" "$url" && return 0
    fi
    rm -f "$output"
    return 1
}

LATEST_TAG=""
if [ "$FORCE_UPDATE" = false ] && [ -f "$VERSION_FILE" ] && [ -x "./$BINARY_NAME" ]; then
    info "Using installed release marker: $(cat "$VERSION_FILE")."
    info "Checking for updates..."
    LATEST_TAG="$(latest_release_tag)"
else
    info "Fetching latest release info..."
    LATEST_TAG="$(latest_release_tag)"

    if [ -z "$LATEST_TAG" ]; then
        err "Could not determine latest release. Check network connection."
        exit 1
    fi
    info "Latest release: $LATEST_TAG"

    ARCHIVE="${ARCHIVE_PREFIX}${LATEST_TAG#v}${ARCHIVE_SUFFIX}"
    DOWNLOAD_URL="https://github.com/$REPO/releases/download/${LATEST_TAG}/${ARCHIVE}"

    info "Downloading $ARCHIVE ..."
    if ! download_file "$DOWNLOAD_URL" "$ARCHIVE"; then
        LEGACY_ARCHIVE="${LEGACY_ARCHIVE_PREFIX}${LATEST_TAG#v}${ARCHIVE_SUFFIX}"
        LEGACY_DOWNLOAD_URL="https://github.com/$REPO/releases/download/${LATEST_TAG}/${LEGACY_ARCHIVE}"
        warn "$ARCHIVE is unavailable; trying the legacy release asset."
        if ! download_file "$LEGACY_DOWNLOAD_URL" "$LEGACY_ARCHIVE"; then
            err "Download failed for both current and legacy Android assets."
            exit 1
        fi
        ARCHIVE="$LEGACY_ARCHIVE"
    fi

    info "Extracting..."
    # Clear the previous install FIRST. Without this an --update kept the old
    # binary: the "is it nested?" test below saw the existing ./$BINARY_NAME,
    # skipped the move, and left the freshly extracted one stranded in its
    # package directory -- while $VERSION_FILE still advanced to the new tag,
    # so the update looked like it worked and never did. Also sweep away any
    # package directory a run of that older script orphaned here.
    rm -f "./$BINARY_NAME" "./$LEGACY_BINARY_NAME"
    find . -maxdepth 1 -type d \( -name "${ARCHIVE_PREFIX}*" -o -name "${LEGACY_ARCHIVE_PREFIX}*" \) \
        -exec rm -rf {} + 2>/dev/null || true

    tar xzf "$ARCHIVE"
    rm -f "$ARCHIVE"

    # The release tarball nests everything under <package>/ (see
    # scripts/release-android.sh); lift the binary out to $INSTALL_DIR.
    if [ ! -f "./$BINARY_NAME" ]; then
        NESTED="$(find . -maxdepth 2 -type f \
            \( -name "$BINARY_NAME" -o -name "$LEGACY_BINARY_NAME" \) | head -1)"
        if [ -n "$NESTED" ]; then
            mv "$NESTED" "./$BINARY_NAME"
            # dirname is never "." here: this branch only runs when
            # ./$BINARY_NAME is absent, so find cannot have matched at depth 1.
            rm -rf "$(dirname "$NESTED")" 2>/dev/null || true
        else
            err "Extraction succeeded but $BINARY_NAME binary not found."
            exit 1
        fi
    fi
    chmod +x "./$BINARY_NAME"
    echo "$LATEST_TAG" > "$VERSION_FILE"
    info "Installed $LATEST_TAG."
fi

if ! BINARY_VERSION="$(./"$BINARY_NAME" --version 2>/dev/null)" ||
   [ -z "$BINARY_VERSION" ]; then
    err "Installed miner could not report its version."
    err "Run this script with --update to repair the installation."
    exit 1
fi

if [ -n "$LATEST_TAG" ] &&
   [ "$BINARY_VERSION" != "Dirtybird C Miner $LATEST_TAG" ] &&
   [ "$BINARY_VERSION" != "Dirtybird Miner $LATEST_TAG" ]; then
    warn "Update available: $BINARY_VERSION -> Dirtybird C Miner $LATEST_TAG."
    warn "Update before benchmarking:"
    note "curl -fsSL https://raw.githubusercontent.com/$REPO/master/scripts/termux-setup.sh | bash -s -- --update"
fi

# ── step 4: pick a daemon / pool ─────────────────────────────────────────────
# Rejected input re-prompts instead of exiting: this is the one interactive step
# a phone user is likely to fat-finger, and the download has already succeeded by
# now -- throwing the whole install away over a typo is a poor trade.
if [ "$RECONFIGURE" = true ] || [ ! -f "config.json" ]; then
    printf "\n"
    printf "${CYAN}Select a daemon/pool:${NC}\n\n"
    for i in "${!DAEMON_NAMES[@]}"; do
        if [ -n "${DAEMON_ADDRS[$i]}" ]; then
            printf "  ${GREEN}[%d]${NC} %-16s %s\n" \
                "$((i + 1))" "${DAEMON_NAMES[$i]}" "${DAEMON_ADDRS[$i]}"
            printf "      %-16s ${YELLOW}%s${NC}\n" "" "${DAEMON_KINDS[$i]}"
        else
            printf "  ${GREEN}[%d]${NC} %s\n" "$((i + 1))" "${DAEMON_NAMES[$i]}"
        fi
    done
    printf "\n"

    POOL=""
    while [ -z "$POOL" ]; do
        read -rp "  Choice [1]: " CHOICE </dev/tty || CHOICE=""
        CHOICE="${CHOICE:-1}"

        if ! printf '%s' "$CHOICE" | grep -qE '^[0-9]+$' ||
           [ "$CHOICE" -lt 1 ] || [ "$CHOICE" -gt "${#DAEMON_NAMES[@]}" ]; then
            warn "Enter a number from 1 to ${#DAEMON_NAMES[@]}."
            continue
        fi

        POOL="${DAEMON_ADDRS[$((CHOICE - 1))]}"
        if [ -z "$POOL" ]; then
            printf "\n"
            printf "${CYAN}Daemon/pool address (host:port, no scheme prefix)${NC}\n"
            read -rp "  Address: " POOL </dev/tty || POOL=""
            # Validated, not just non-empty: this string is interpolated straight
            # into the config.json heredoc below, so a quote or newline would
            # emit malformed JSON the miner then silently fails to parse.
            if ! printf '%s' "$POOL" | grep -qE '^[A-Za-z0-9._-]+:[0-9]{1,5}$'; then
                warn "Expected host:port (e.g. dero-node.net:10100)."
                POOL=""
            fi
        fi
    done
    info "Using: $POOL"

    # ── step 5: prompt for wallet address ────────────────────────────────────
    printf "\n"
    printf "${CYAN}DERO wallet address${NC}\n"
    printf "  Press Enter to use: ${GREEN}%s${NC}\n" "$DEFAULT_WALLET"
    read -rp "  Wallet: " INPUT_WALLET </dev/tty
    WALLET="${INPUT_WALLET:-$DEFAULT_WALLET}"

    # Validate wallet format. The length floor matters: the miner itself only
    # checks the address is non-empty, so this regex is the sole guard against
    # a truncated paste -- which otherwise mines to nobody until someone
    # notices hours later. A real address is 66 characters.
    if ! printf '%s' "$WALLET" | grep -qE '^(dero1|deto1)[a-z0-9]{60,}$'; then
        err "Invalid wallet address: $WALLET"
        err "Must start with 'dero1' or 'deto1' followed by 60+ lowercase alphanumerics."
        exit 1
    fi

    # ── step 6: choose threads (nproc - 1 by default, minimum 1) ────────────
    CORES="$(nproc 2>/dev/null || echo 4)"
    DEFAULT_THREADS=$((CORES - 1))
    [ "$DEFAULT_THREADS" -lt 1 ] && DEFAULT_THREADS=1

    printf "\n"
    printf "${CYAN}Mining threads${NC}\n"
    while true; do
        read -rp "  Threads [${DEFAULT_THREADS}] (1-${CORES}): " INPUT_THREADS </dev/tty ||
            INPUT_THREADS=""
        INPUT_THREADS="${INPUT_THREADS:-$DEFAULT_THREADS}"
        if printf '%s' "$INPUT_THREADS" | grep -qE '^[1-9][0-9]*$' &&
           [ "$INPUT_THREADS" -le "$CORES" ]; then
            THREADS="$INPUT_THREADS"
            break
        fi
        warn "Enter a number from 1 to $CORES."
    done

    # ── step 7: write config.json ────────────────────────────────────────────
    cat > config.json <<EOF
{
  "daemon-address": "$POOL",
  "wallet": "$WALLET",
  "threads": $THREADS,
  "priority": "normal"
}
EOF
    info "Config written to $INSTALL_DIR/config.json"
else
    info "Using existing config.json (use --reconfigure to change)."
fi

# ── step 8: battery advisory ──────────────────────────────────────────────────
# Every termux-* call is wrapped in `timeout`: `command -v` only proves the
# termux-api PACKAGE is installed, not that the companion APP is present. With
# the package but no app, these block indefinitely -- and this one runs before
# mining starts, so the phone would sit idle with no output and no error.
if command -v termux-battery-status &>/dev/null; then
    BATTERY_JSON="$(timeout 5 termux-battery-status 2>/dev/null || true)"
    BAT_PCT="$(printf '%s' "$BATTERY_JSON" | jq -r '.percentage // empty' 2>/dev/null || true)"
    BAT_PLUGGED="$(printf '%s' "$BATTERY_JSON" | jq -r '.plugged // empty' 2>/dev/null || true)"
    if [ -n "$BAT_PCT" ] && [ "$BAT_PCT" -lt 40 ] 2>/dev/null; then
        warn "Battery is ${BAT_PCT}%. Mining drains battery fast; consider charging."
    fi
    if [ "$BAT_PLUGGED" = "UNPLUGGED" ]; then
        warn "Device is running on battery power. Mining drains battery fast."
    fi
fi

# ── step 9: acquire wake-lock ────────────────────────────────────────────────
WAKE_LOCK=false
if command -v termux-wake-lock &>/dev/null; then
    if timeout 5 termux-wake-lock 2>/dev/null; then
        WAKE_LOCK=true
        info "Wake-lock acquired (Android Doze will not suspend the miner)."
    else
        warn "Could not acquire wake-lock. Android Doze may pause the miner in background."
    fi
else
    note "Install termux-api + 'pkg install termux-api' for wake-lock support."
    note "Without it, Android Doze may pause the miner in background."
fi

# ── step 10: run with auto-restart ────────────────────────────────────────────
printf "\n"
printf "  Version:  ${GREEN}%s${NC}\n" "$BINARY_VERSION"
printf "  Pool:     ${GREEN}%s${NC}\n" "$(jq -r '.["daemon-address"]' config.json)"
printf "  Wallet:   ${GREEN}%s${NC}\n" "$(jq -r '.wallet' config.json)"
printf "  Threads:  ${GREEN}%s${NC}\n" "$(jq -r '.threads' config.json)"
printf "  Priority: ${GREEN}%s${NC}\n" "$(jq -r '.priority' config.json)"
printf "\n"
info "Starting miner... (Ctrl-C to stop)"
printf "\n"

release_lock() {
    if [ "$WAKE_LOCK" = true ]; then
        timeout 5 termux-wake-unlock 2>/dev/null || true
        info "Wake-lock released."
    fi
}
trap release_lock EXIT INT TERM

BACKOFF=5
MAX_BACKOFF=30
while true; do
    set +e
    ./"$BINARY_NAME"
    EXIT_CODE=$?
    set -e
    if [ "$EXIT_CODE" -eq 0 ]; then
        info "Miner exited cleanly."
        break
    fi
    warn "Miner exited with code $EXIT_CODE. Restarting in ${BACKOFF}s..."
    sleep "$BACKOFF"
    BACKOFF=$((BACKOFF * 2))
    [ "$BACKOFF" -gt "$MAX_BACKOFF" ] && BACKOFF="$MAX_BACKOFF"
done
