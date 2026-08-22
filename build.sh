#!/bin/bash
# DIRTYBIRD C Miner — Native build script for MSYS2/MinGW64 (clang + Ninja)
# Run from the source root (the directory containing this script).
#
# Usage:
#   ./build.sh release            # standard release (default)
#   ./build.sh pgo-gen            # PGO instrumentation trainer build (step 1)
#   ./build.sh pgo-use            # PGO-optimized build (step 2, after training)
#   ./build.sh bench              # best local build for the detected CPU
#
# CPU tuning knobs (pass as env vars, e.g.  DLUNA_MTUNE=znver3 ./build.sh release):
#   DLUNA_MTUNE      -mtune= target for x86-64 (default: raptorlake, i.e. the
#                     project's tested baseline; on AMD Zen 3/4 use znver3/znver4)
#   DLUNA_VECTORIZE  1  -> drop -fno-vectorize/-fno-slp-vectorize (clang auto-vec)
#   DLUNA_SPSA=0     -> disable the Tritonn SPSA verification path (smaller workers)
#
# PGO requires: mingw-w64-x86_64-clang, mingw-w64-x86_64-compiler-rt,
#               mingw-w64-x86_64-llvm (for llvm-profdata). CMake auto-finds the
#               clang PGO runtime lib (MSYS2 layout quirk handled in CMakeLists).

set -e

export PATH="/c/msys64/mingw64/bin:$PATH"

MODE="${1:-release}"
JOBS="${2:-2}"  # PGO link needs -j2

EXTRA=()
[ -n "$DLUNA_MTUNE" ]     && EXTRA+=("-DDLUNA_MTUNE=$DLUNA_MTUNE")
[ -n "$DLUNA_VECTORIZE" ] && EXTRA+=("-DDLUNA_VECTORIZE=$DLUNA_VECTORIZE")
[ -n "$DLUNA_SPSA" ]      && EXTRA+=("-DDLUNA_SPSA=$DLUNA_SPSA")

case "$MODE" in
  release|bench)
    echo "=== Building: Release (LTO + x86-64-v3, flags: ${EXTRA[*]:-default}) ==="
    rm -rf build && mkdir build && cd build
    cmake -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      "${EXTRA[@]}" \
      ..
    cmake --build . -j"$JOBS"
    echo ""
    echo "=== Binary: $(pwd)/bin/dirtybird-c-miner.exe ==="
    ls -la bin/dirtybird-c-miner.exe
    ;;

  pgo-gen)
    echo "=== Building: PGO Generate (step 1) ==="
    rm -rf build-pgo-gen && mkdir build-pgo-gen && cd build-pgo-gen
    cmake -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DPGO_GENERATE=ON \
      "${EXTRA[@]}" \
      ..
    cmake --build . --target dirtybird-c-pgo-train -j"$JOBS"
    echo ""
    echo "=== PGO instrumented trainer ready ==="
    echo "Collect profile data:"
    echo "  LLVM_PROFILE_FILE=\"pgo-%p.profraw\" ./bin/dirtybird-c-pgo-train.exe -t 20 --seconds 60 --rotate-ms 5000 --difficulty 1000000000"
    echo "Then run: ./build.sh pgo-use"
    ;;

  pgo-use)
    echo "=== Building: PGO Use (step 2) ==="
    cd build-pgo-gen
    shopt -s nullglob
    profiles=( *.profraw )
    shopt -u nullglob
    if [ "${#profiles[@]}" -eq 0 ]; then
      echo "ERROR: No profile data found. Run dirtybird-c-pgo-train.exe from build-pgo-gen first!"
      exit 1
    fi
    llvm-profdata merge -o merged.profdata "${profiles[@]}"
    cd ..
    rm -rf build-pgo-use && mkdir build-pgo-use && cd build-pgo-use
    # CMake needs a native Windows path (C:/...) — MSYS /c/... isn't absolute to it.
    PROFILE_NATIVE="$(cygpath -m "$(pwd)/../build-pgo-gen/merged.profdata")"
    cmake -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DPGO_USE=ON \
      -DPGO_PROFILE="$PROFILE_NATIVE" \
      "${EXTRA[@]}" \
      ..
    cmake --build . -j"$JOBS"
    echo ""
    echo "=== PGO-optimized binary ready ==="
    echo "Binary: $(pwd)/bin/dirtybird-c-miner.exe"
    ls -la bin/dirtybird-c-miner.exe
    ;;

  *)
    echo "Usage: $0 [release|pgo-gen|pgo-use|bench] [jobs]"
    exit 1
    ;;
esac

echo ""
echo "Test run:"
echo "  ./bin/dirtybird-c-miner.exe -d 127.0.0.1:10100 -w YOUR_WALLET_ADDRESS -t 20"
echo "  (add -p max on a headless box for the full-clock profile)"
