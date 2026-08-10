#!/usr/bin/env bash
. "$(dirname "${BASH_SOURCE[0]}")/h-manifest.conf"

# NOTE: dirtybird-c-miner has no HTTP stats API yet. Until --api-listen lands,
# HiveOS shows the rig as running with 0 H/s. Mining is unaffected.
khs=0
stats=$(cat <<-END
{
    "hs": [0],
    "hs_units": "khs",
    "uptime": 0,
    "ar": [0, 0],
    "algo": "ASTROBWT"
}
END
)
