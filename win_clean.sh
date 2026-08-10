#!/bin/bash
# Build a clean Windows LTO+PGO trainer, let the box COOL, then measure sustained
# hashrate at the real thread counts -- to reconcile vs the user's ~19-22 KH/s.
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /f/dero-miner-main/dirtybird-fix
log(){ echo "[$(date +%H:%M:%S)] $*"; }
DIFF=1000000000
khof(){ grep -oE 'hashes=[0-9]+' | tail -1 | cut -d= -f2; }

log "build LTO+PGO trainer (TARGET=trainer, 60s profile)"
export CMAKE_ARGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DDIRTYBIRD_C_VERSION=0.0.0-clean"
export TARGET=dirtybird-c-pgo-train TRAIN_SECS=60 BUILD_DIR=build-clean GENERATOR="MinGW Makefiles"
bash scripts/build-pgo.sh > win_clean_build.log 2>&1
T=./build-clean/bin/dirtybird-c-pgo-train.exe
[ -x "$T" ] || { log "trainer build FAILED"; tail -15 win_clean_build.log; exit 1; }
log "trainer built: $(ls -la $T | awk '{print $5}') bytes"

log "COOLING 180s (box was heat-soaked)..."; sleep 180

for run in 1 2; do
  for t in 20 24; do
    H=$("$T" -t $t --seconds 60 --difficulty $DIFF --rotate-ms 9000 2>&1 | khof)
    log "run$run  -t $t : $(awk "BEGIN{printf \"%.2f\",${H:-0}/60/1000}") KH/s"
    sleep 20
  done
done
log "=== DONE ==="
