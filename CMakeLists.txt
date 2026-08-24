# DIRTYBIRD C Miner -- CMake build
#
# Clean build:
#   mkdir build && cd build
#   cmake -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ..
#   cmake --build . -j$(nproc)
#
# PGO build (two-pass, local hashrate candidate):
#   mkdir build-pgo-gen && cd build-pgo-gen
#   cmake -DPGO_GENERATE=ON ..
#   cmake --build . --target dirtybird-c-pgo-train -j2
#   LLVM_PROFILE_FILE="pgo-%p.profraw" ./dirtybird-c-pgo-train -t 20 --seconds 60
#   llvm-profdata merge -o merged.profdata *.profraw
#   cd .. && mkdir build-pgo-use && cd build-pgo-use
#   cmake -DPGO_USE=ON -DPGO_PROFILE=../build-pgo-gen/merged.profdata ..
#   cmake --build . -j$(nproc)

cmake_minimum_required(VERSION 3.16)
project(dirtybird-c-miner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Version (drives the --version/banner string; release CI passes -DDIRTYBIRD_C_VERSION=<tag>) ----
set(DIRTYBIRD_C_VERSION_DEFAULT "dev")
if(DEFINED DIRTYBIRD_C_VERSION)
    set(INTERNAL_DIRTYBIRD_C_VERSION "${DIRTYBIRD_C_VERSION}")
elseif(DEFINED ENV{DIRTYBIRD_C_VERSION})
    set(INTERNAL_DIRTYBIRD_C_VERSION "$ENV{DIRTYBIRD_C_VERSION}")
else()
    set(INTERNAL_DIRTYBIRD_C_VERSION "${DIRTYBIRD_C_VERSION_DEFAULT}")
endif()
string(REGEX REPLACE "^v" "" INTERNAL_DIRTYBIRD_C_VERSION "${INTERNAL_DIRTYBIRD_C_VERSION}")
message(STATUS "DIRTYBIRD version: ${INTERNAL_DIRTYBIRD_C_VERSION}")

# Release packaging expects executables under build/bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Compiler detection ----
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(IS_CLANG TRUE)
else()
    set(IS_CLANG FALSE)
endif()

# MinGW CMake can leave this empty outside a developer shell.
if(NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
endif()

# ---- Base optimization flags (arch-aware) ----
# x86-64: AVX2 baseline tuned for Raptor Lake; auto-vec off (icache/thermal, verified).
# aarch64: ARMv8-A + crypto extensions (SHA/AES); SHA uses OpenSSL's
#          runtime-dispatched ARM implementation, with no x86 intrinsics.
# -flto: +7.4% sustained on x86 (enabled by default for Release).
# -O3 base: measured equivalent to -O2 under LTO+2-pass-PGO (2026-07-25,
# 3x145s interleaved A/B on i7-13700HX @20T: 19.21 vs 19.08 KH/s, orderings
# disagree = noise). The old "-O2 proven winner (icache)" finding predates
# PGO; text grew only +5KB at -O3. Flag choice is now evidence-backed.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[0-9]86)$")
    # CPU tuning knobs. Defaults reproduce the historical build exactly
    # (raptorlake tune + vectorization off — that was verified on a Raptor
    # Lake i7-13700HX). On other silicon (e.g. AMD Zen 3/4) pass
    # -DDLUNA_MTUNE=znver3 and/or -DDLUNA_VECTORIZE=ON and measure.
    set(DLUNA_MTUNE "raptorlake" CACHE STRING "-mtune= target for x86-64 builds")
    set(DLUNA_VECTORIZE OFF CACHE BOOL "Enable clang auto-vectorization (default OFF)")
    set(_DLUNA_NO_VEC "-fno-vectorize -fno-slp-vectorize")
    if(DLUNA_VECTORIZE)
        set(_DLUNA_NO_VEC "")
    endif()
    set(BASE_CXX_FLAGS "-O3 -DNDEBUG -march=x86-64-v3 -mtune=${DLUNA_MTUNE} -fomit-frame-pointer -mavx2 ${_DLUNA_NO_VEC} -finline-functions")
    set(BASE_C_FLAGS   "-O3 -DNDEBUG -march=x86-64-v3 -mtune=${DLUNA_MTUNE} -fomit-frame-pointer -mavx2 ${_DLUNA_NO_VEC}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(BASE_CXX_FLAGS "-O3 -DNDEBUG -march=armv8-a+crypto -fomit-frame-pointer -finline-functions")
    set(BASE_C_FLAGS   "-O3 -DNDEBUG -march=armv8-a+crypto -fomit-frame-pointer")
else()
    set(BASE_CXX_FLAGS "-O3 -DNDEBUG -fomit-frame-pointer")
    set(BASE_C_FLAGS   "-O3 -DNDEBUG -fomit-frame-pointer")
endif()

# ---- LTO (Enabled by default for best performance) ----
option(DISABLE_LTO "Disable link-time optimization" OFF)
if(NOT DISABLE_LTO)
    set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -flto")
    set(BASE_C_FLAGS   "${BASE_C_FLAGS} -flto")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
    # Apple's ld64 handles LTO natively via libLTO and rejects -fuse-ld=lld.
    if(IS_CLANG AND NOT APPLE)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
    endif()
endif()

# ---- Clang PGO runtime lib search path (MSYS2/MinGW quirk) ----
# `clang -fprofile-generate` looks for libclang_rt.profile under
#   <clang-resource-dir>/lib/<triple>/  (e.g. .../lib/x86_64-w64-windows-gnu/)
# but MSYS2's mingw-w64-x86_64-compiler-rt installs it under
#   <clang-resource-dir>/lib/windows/
# so the stock PGO build fails to link with "could not open
# libclang_rt.profile.a". Resolve the real resource dir at configure time and
# add the location that actually exists. No-op where the layout is standard.
if(PGO_GENERATE OR PGO_USE)
    if(IS_CLANG)
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
            OUTPUT_VARIABLE CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(CLANG_RESOURCE_DIR)
            file(GLOB _CLANG_PGO_RT
                "${CLANG_RESOURCE_DIR}/lib/*/libclang_rt.profile*.a")
            if(_CLANG_PGO_RT)
                list(GET _CLANG_PGO_RT 0 _RT_PATH)
                get_filename_component(_RT_DIR "${_RT_PATH}" DIRECTORY)
                set(CMAKE_EXE_LINKER_FLAGS
                    "${CMAKE_EXE_LINKER_FLAGS} -L\"${_RT_DIR}\"")
                message(STATUS "PGO runtime lib: ${_RT_PATH}")
            endif()
        endif()
    endif()
endif()

# ---- PGO ----
option(PGO_GENERATE "Build with PGO instrumentation" OFF)
option(PGO_USE "Build with PGO optimization" OFF)
set(PGO_PROFILE "" CACHE FILEPATH "Path to merged.profdata for PGO use")
set(PGO_PROFILE_RESOLVED "")

if(PGO_GENERATE AND PGO_USE)
    message(FATAL_ERROR "PGO_GENERATE and PGO_USE are mutually exclusive")
endif()
if(PGO_USE)
    if(NOT PGO_PROFILE)
        message(FATAL_ERROR "PGO_USE requires -DPGO_PROFILE=/path/to/merged.profdata")
    endif()
    if(IS_ABSOLUTE "${PGO_PROFILE}")
        set(PGO_PROFILE_RESOLVED "${PGO_PROFILE}")
    elseif(EXISTS "${CMAKE_BINARY_DIR}/${PGO_PROFILE}")
        get_filename_component(PGO_PROFILE_RESOLVED
            "${CMAKE_BINARY_DIR}/${PGO_PROFILE}" ABSOLUTE)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/${PGO_PROFILE}")
        get_filename_component(PGO_PROFILE_RESOLVED
            "${CMAKE_SOURCE_DIR}/${PGO_PROFILE}" ABSOLUTE)
    else()
        set(PGO_PROFILE_RESOLVED "${PGO_PROFILE}")
    endif()
    if(NOT EXISTS "${PGO_PROFILE_RESOLVED}")
        message(FATAL_ERROR "PGO_PROFILE does not exist: ${PGO_PROFILE}")
    endif()
endif()

if(PGO_GENERATE)
    if(IS_CLANG)
        set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -fprofile-generate -DDLUNA_PGO_GENERATE")
        set(BASE_C_FLAGS   "${BASE_C_FLAGS} -fprofile-generate")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate")
    else()
        set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -fprofile-generate -DDLUNA_PGO_GENERATE")
        set(BASE_C_FLAGS   "${BASE_C_FLAGS} -fprofile-generate")
    endif()
elseif(PGO_USE)
    if(IS_CLANG)
        set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -fprofile-use=${PGO_PROFILE_RESOLVED}")
        set(BASE_C_FLAGS   "${BASE_C_FLAGS} -fprofile-use=${PGO_PROFILE_RESOLVED}")
    else()
        set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -fprofile-use=${PGO_PROFILE_RESOLVED} -fprofile-correction")
        set(BASE_C_FLAGS   "${BASE_C_FLAGS} -fprofile-use=${PGO_PROFILE_RESOLVED} -fprofile-correction")
    endif()
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${BASE_CXX_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE   "${BASE_C_FLAGS}")

# ---- SHA-NI wrap flags ----
# Redirect OpenSSL SHA256 calls to our SHA-NI hardware implementation.
# (A legacy -Wl,--wrap=nanosleep lived here too; it neutralised
# std::this_thread::sleep_for and broke every POSIX dluna_sleep_ms --
# see test sleep_ms. Nothing linked here calls nanosleep anymore.)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[0-9]86)$")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--wrap=SHA256_Init -Wl,--wrap=SHA256_Update -Wl,--wrap=SHA256_Final")
endif()

# USE_ASTRO_SPSA gates the x86-only, AVX2 Tritonn verification path (and its
# extra workerData fields). The live hash path (descriptor-SA / libsais /
# wolfBranch) does not need it; gating it off is a tested config (Δ-0.31 KH/s
# on i7-13700HX, within thermal noise). It also drops workerData from ~1.15 MB
# to ~360 KB — a real L3 working-set win on dual-CCD AM4/AM5 (2×32MB L3) where
# 24 x2 workers at 1.15 MB each exceed L3. Tunable: -DDLUNA_SPSA=OFF.
# Off on non-x86 so the tree cross-compiles for aarch64.
option(DLUNA_SPSA "Enable the USE_ASTRO_SPSA Tritonn verification path" ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[0-9]86)$" AND DLUNA_SPSA)
    add_definitions(-DUSE_ASTRO_SPSA=1)
endif()

# ---- OpenSSL ----
find_package(OpenSSL REQUIRED)
enable_testing()

function(dluna_copy_runtime_dlls target_name)
    if(WIN32)
        get_filename_component(DLUNA_RUNTIME_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(DLUNA_RUNTIME_DLL
            libcrypto-3-x64.dll
            libgcc_s_seh-1.dll
            libssl-3-x64.dll
            libstdc++-6.dll
            libwinpthread-1.dll
        )
            if(EXISTS "${DLUNA_RUNTIME_BIN_DIR}/${DLUNA_RUNTIME_DLL}")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${DLUNA_RUNTIME_BIN_DIR}/${DLUNA_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:${target_name}>/${DLUNA_RUNTIME_DLL}"
                    COMMENT "Copying ${DLUNA_RUNTIME_DLL} for ${target_name}"
                )
            endif()
        endforeach()
    endif()
endfunction()

# ---- Sources ----
# 2026-04-30 Phase B technique #1 reverted: gating USE_ASTRO_SPSA off
# measured Δ -0.31 KH/s sustained (within thermal noise, failed +1 KH/s
# gate per perf-study-2026-04-30/before-tech1-gate-spsa.log vs after-).
# The #if gates added to astroworker.h / astrobwt.cpp / main.cpp / miner.cpp
# are kept (harmless when USE_ASTRO_SPSA defined; useful for future ports).
# ---- libdluna_stages: stage code shared between mining exe and replay harness ----
# OBJECT (not STATIC): under -flto the members are LLVM bitcode, and a GNU-ar
# archive's symbol index omits bitcode symbols, so lld can't pull members and the
# link fails with "undefined symbol" on toolchains without llvm-ar (MinGW/Windows
# CI). OBJECT libraries feed the object files straight into each consumer's link,
# so there is no archive and no bitcode-index dependency on any platform.
add_library(dluna_stages OBJECT
    src/astrobwt.cpp
    src/branch.cpp
    src/salsa20.cpp
    src/libsais.c
    src/spsa.cpp
    src/spsa_local.cpp
    src/sa_2byte_radix.cpp
    src/dluna_stages.cpp
    src/v114_stubs.cpp
)

# SHA-NI and the 2-way batched SHA are part of the live hash path on x86.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[0-9]86)$")
    target_sources(dluna_stages PRIVATE
        src/sha256_ni.cpp
        src/sha256_x2.cpp
    )
endif()

# Tritonn verification path uses AVX2 helpers and is referenced only under
# USE_ASTRO_SPSA (x86-only, above). Compile it on x86 only when the SPSA path
# is enabled, so non-x86 and -DDLUNA_SPSA=OFF builds don't pull in
# <immintrin.h> or the SPSA-only workerData fields.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[0-9]86)$" AND DLUNA_SPSA)
    target_sources(dluna_stages PRIVATE
        src/spsa_tritonn.cpp
        src/spsa_tritonn_helpers.cpp
    )
endif()

target_include_directories(dluna_stages PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dluna_stages PUBLIC OpenSSL::Crypto OpenSSL::SSL)

add_library(dluna_runtime OBJECT
    src/runtime_tune.cpp
    src/console.cpp
    src/cpu_topology.cpp
)
target_include_directories(dluna_runtime PUBLIC ${CMAKE_SOURCE_DIR}/include)
# console.cpp -> astroworker.h -> <openssl/sha.h>: needs the OpenSSL include path
# (matters when OpenSSL is in a custom prefix, e.g. the Android NDK cross-build).
target_link_libraries(dluna_runtime PUBLIC OpenSSL::Crypto)

# ---- Mining executable: thin wrapper around the lib ----
add_executable(dirtybird-c-miner
    src/main.cpp
    src/miner.cpp
    src/network.cpp
    src/difficulty.cpp
)

# pthread must be linked explicitly: on glibc < 2.34 (e.g. the Ubuntu 20.04
# baseline HiveOS runs on) pthread lives in libpthread.so, not merged into libc.
find_package(Threads REQUIRED)

target_link_libraries(dirtybird-c-miner PRIVATE dluna_stages dluna_runtime Threads::Threads)
target_compile_definitions(dirtybird-c-miner PRIVATE DIRTYBIRD_C_VERSION_STR="${INTERNAL_DIRTYBIRD_C_VERSION}")
dluna_copy_runtime_dlls(dirtybird-c-miner)
add_test(NAME dirtybird-c-selftest COMMAND dirtybird-c-miner --selftest)

add_executable(dirtybird-c-pgo-train
    tools/pgo_trainer.cpp
    src/miner.cpp
    src/difficulty.cpp
)
target_include_directories(dirtybird-c-pgo-train PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dirtybird-c-pgo-train PRIVATE dluna_stages dluna_runtime Threads::Threads)
dluna_copy_runtime_dlls(dirtybird-c-pgo-train)

# 2026-04-29: phase-A profiling showed libsais is 90% of hash time. Bump it
# to -O3 for aggressive vectorization on its hot inner loops. (The base level
# is now also -O3 — see top of file — so this pin is redundant, kept as
# documentation of the hot spot.)
set_source_files_properties(src/libsais.c PROPERTIES COMPILE_FLAGS "-O3")

# ---- SPSA: bundled in-tree clean-room implementation ----
# The SPSA stage is built from the in-tree sources above (no external
# library dependency), so source builds are fully self-contained.

# ---- Platform libs ----
if(WIN32)
    target_link_libraries(dirtybird-c-miner PRIVATE ws2_32)
endif()

add_subdirectory(tools/replay)

# ---- Strip release binary ----
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT PGO_GENERATE)
    add_custom_command(TARGET dirtybird-c-miner POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:dirtybird-c-miner> || true
        COMMENT "Stripping debug symbols"
    )
endif()
