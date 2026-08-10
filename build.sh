#!/bin/bash
# DIRTYBIRD C Miner — Native build script for MSYS2/MinGW64
# Run from the source root (the directory containing this script).
#
# Usage:
#   ./build.sh          # Standard -O2 + LTO build
#   ./build.sh pgo-gen  # PGO instrumentation trainer build (step 1)
#   ./build.sh pgo-use  # PGO optimized build (step 2, after trainer run)

set -e

export PATH="/c/msys64/mingw64/bin:$PATH"

MODE="${1:-release}"
JOBS="${2:-2}"  # PGO link needs -j2

case "$MODE" in
  release)
    echo "=== Building: Release (-O2 + LTO + x86-64-v3) ==="
    rm -rf build && mkdir build && cd build
    cmake -G "MinGW Makefiles" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      ..
    cmake --build . -j"$JOBS"
    echo ""
    echo "=== Binary: $(pwd)/dirtybird-c-miner.exe ==="
    ls -la dirtybird-c-miner.exe
    ;;

  pgo-gen)
    echo "=== Building: PGO Generate (step 1) ==="
    rm -rf build-pgo-gen && mkdir build-pgo-gen && cd build-pgo-gen
    cmake -G "MinGW Makefiles" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DPGO_GENERATE=ON \
      ..
    cmake --build . --target dirtybird-c-pgo-train -j"$JOBS"
    echo ""
    echo "=== PGO instrumented trainer ready ==="
    echo "Collect profile data:"
    echo "  LLVM_PROFILE_FILE=\"pgo-%p.profraw\" ./dirtybird-c-pgo-train.exe -t 20 --seconds 60 --rotate-ms 5000 --difficulty 1000000000"
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
    cmake -G "MinGW Makefiles" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DPGO_USE=ON \
      -DPGO_PROFILE="$(pwd)/../build-pgo-gen/merged.profdata" \
      ..
    cmake --build . -j"$JOBS"
    echo ""
    echo "=== PGO-optimized binary ready ==="
    echo "Binary: $(pwd)/dirtybird-c-miner.exe"
    ls -la dirtybird-c-miner.exe
    ;;

  *)
    echo "Usage: $0 [release|pgo-gen|pgo-use] [jobs]"
    exit 1
    ;;
esac

echo ""
echo "Test run:"
echo "  ./dirtybird-c-miner.exe -d 127.0.0.1:10100 -w YOUR_WALLET_ADDRESS -t 20"
