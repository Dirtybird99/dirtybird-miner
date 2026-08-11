#!/usr/bin/env bash
. "$(dirname "${BASH_SOURCE[0]}")/h-manifest.conf"

# dirtybird-c-miner has no HTTP stats API yet; instead it emits one
# machine-parseable status line per second on its redirected stdout (the
# "that stream is machine-parsed (HiveOS)" branch in src/main.cpp). h-run.sh
# tees that stream into ${CUSTOM_LOG_BASENAME}.log, so parse the latest line
# from there. Pre-first-job / disconnected the line still prints with a real
# 0.00, so HiveOS shows a live zero rather than stale data.
LOG="${CUSTOM_LOG_BASENAME}.log"
STATUS_RE='\[DIRTYBIRD-C\] [0-9.]+ KH/s'

line="$(grep -E "${STATUS_RE}" "$LOG" 2>/dev/null | tail -n 1)"

rate=0; accepted=0; rejected=0; uptime=0
if [[ -n "$line" ]]; then
    rate="$(sed -nE 's/.*\[DIRTYBIRD-C\] ([0-9.]+) KH\/s.*/\1/p' <<<"$line")"
    accepted="$(sed -nE 's/.*Miniblocks:([0-9]+).*/\1/p' <<<"$line")"
    rejected="$(sed -nE 's/.*REJ:([0-9]+).*/\1/p' <<<"$line")"
    hhmmss="$(sed -nE 's/.*\| ([0-9]{2}):([0-9]{2}):([0-9]{2})$/\1 \2 \3/p' <<<"$line")"
    if [[ -n "$hhmmss" ]]; then
        read -r h m s <<<"$hhmmss"
        uptime=$((10#$h*3600 + 10#$m*60 + 10#$s))
    fi
fi

[[ "$rate" =~ ^[0-9.]+$ ]] || rate=0
[[ "$accepted" =~ ^[0-9]+$ ]] || accepted=0
[[ "$rejected" =~ ^[0-9]+$ ]] || rejected=0

khs="$rate"
stats=$(cat <<-END
{
    "hs": [$rate],
    "hs_units": "khs",
    "uptime": $uptime,
    "ar": [$accepted, $rejected],
    "algo": "ASTROBWT",
    "ver": "__VERSION__"
}
END
)
