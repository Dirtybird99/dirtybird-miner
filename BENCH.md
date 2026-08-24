# Benchmarks & tuning (Ryzen 9 5900X, 12C/24T, 2×32MB L3)

Measured on a Ryzen 9 5900X @ 24 threads with `dirtybird-c-pgo-train`
(`--seconds 20..45`, warmup 3-5s, difficulty 1e9), interleaved A/B rounds to
separate thermal noise from signal.

## Results

| Build / config | Mean KH/s | Δ vs baseline |
|---|---|---|
| Baseline (stock flags: `-mtune=raptorlake`, vec off) | 29.2 | — |
| `-DDLUNA_MTUNE=znver3` | 31.2 | +6.9% |
| `-DDLUNA_VECTORIZE=ON` | 32.0 | +9.6% |
| znver3 + vectorize | 33.3 | +14% |
| **znver3 + vectorize + PGO** | **41.1** | **+41%** |
| **+ `-p max` (or `DLUNA_PRIORITY=max`)** | **43.5** | **+49%** |

Long-run confirmation of the final config (3×45s): **43.0 / 44.3 / 42.9 KH/s**.
Live pool (dero.rabidmining.com:10100, mainnet height 7,512,047): **42.6 KH/s,
0 rejects**.

Thread sweep (final PGO build, `-p max`):

| Threads | KH/s |
|---|---|
| 16 | 32.7 |
| 20 | 37.5 |
| **24** | **43.6** |
| 28 | 43.3 |

24 threads is the sweet spot on this box (SMT saturation beyond that).

## How to reproduce

```bash
# 1. one-shot release with the CPU-specific knobs
DLUNA_MTUNE=znver3 DLUNA_VECTORIZE=1 ./build.sh release

# 2. two-pass PGO (biggest lever: +24% on top of znver3+vec)
DLUNA_MTUNE=znver3 DLUNA_VECTORIZE=1 ./build.sh pgo-gen
cd build-pgo-gen
LLVM_PROFILE_FILE="pgo-%p.profraw" ./bin/dirtybird-c-pgo-train.exe -t 24 --seconds 60 --rotate-ms 3000 --difficulty 1000000000
cd ..
DLUNA_MTUNE=znver3 DLUNA_VECTORIZE=1 ./build.sh pgo-use

# 3. run it at full clock (headless)
./build-pgo-use/bin/dirtybird-c-miner.exe -d <pool:port> -w <wallet> -t 24 -p max
```

PGO prerequisites (MSYS2/MinGW64): `mingw-w64-x86_64-clang`,
`mingw-w64-x86_64-compiler-rt`, `mingw-w64-x86_64-llvm` (llvm-profdata).
CMake now auto-locates the clang PGO runtime lib — no manual `-L` needed.

## Verification (the speedup is real, and the work is unchanged)

Three independent confirmations, all on the same 5900X:

### 1. Cause — the compiled artifacts genuinely differ by the tuning flags

Extracted from each build's `build.ninja`:

| | stock | tuned + PGO |
|---|---|---|
| `-mtune=` | `raptorlake` | `znver3` |
| `-fno-vectorize -fno-slp-vectorize` | present | absent |
| `-fprofile-use=` | — | `.../merged.profdata` |

### 2. Same work — byte-identical digests over 2000 nonces

A harness (`samedigest.cpp`) linked against **each build's own object files**
runs the full pipeline (SHA-256 → Salsa20 → RC4 → 278× wolf-compute →
descriptor-SA → SHA-256-over-SA) over 2000 identical nonces and prints a
rolling FNV-1a digest of all 2000 output hashes:

```
STOCK: count=2000 digest=be73943ed6280b5f
PGO:   count=2000 digest=be73943ed6280b5f
```

Identical digests ⇒ the PGO build computes **exactly the same hashes** — it
is not skipping, altering, or shortcutting any work. (The stock binary's own
`--selftest` and the PGO binary's `--selftest` both pass `pow("a") =
54e2324d...` as well.)

### 3. Speed — identical work completes faster, single-threaded

Same 2000 hashes, one thread, only compiler flags differ:

```
STOCK: 2000 hashes in 1.093s
PGO:   2000 hashes in 0.937s   →  -14.3% time, +16.6% throughput
```

### Controlled stock-vs-PGO comparison (12 threads, interleaved, `-p max`)

Both built from the same source tree with only the tuning diff applied
(controlled experiment — no code changes):

| Build | R1 | R2 | R3 | Mean | Δ |
|---|---|---|---|---|---|
| stock (from-source, stock flags) | 22.55 | 22.91 | 21.82 | **22.43** | — |
| tuned (znver3 + vec) | 25.59 | 25.58 | 25.75 | **25.64** | +14.3% |
| tuned + PGO | 30.02 | 29.53 | 30.56 | **30.04** | +33.9% |

Per-core: stock 1.87 KH/s → tuned+PGO 2.50 KH/s (+34%).

The single-thread delta (+16.6%) is smaller than the 12T delta (+33.9%):
under load the vectorized SA-build loops get more memory-pipeline headroom,
so the PGO+vectorize benefit compounds. The 1T wall-time is the cleanest
controlled proof (no SMT/L3 contention) and confirms a real per-hash speedup
on identical work.

## Notes / caveats

- The default `-mtune=raptorlake` + `-fno-vectorize` flags were verified on a
  Raptor Lake i7-13700HX and are kept as the defaults so Intel builds don't
  regress. On AMD Zen 3/4 the tuning above is worth ~+49%.
- `-DDLUNA_SPSA=OFF` drops workerData from ~1.15 MB to ~360 KB (better L3 fit
  on dual-CCD AM4/AM5) and measured neutral here; kept as a tunable.
- AstroBWTv3's RC4 chain is latency-bound and intentionally GPU-hostile
  (see derohe `astrobwt/astrobwtv3/pow.go` comments); the 3090's bandwidth is
  not usable. CPU-only remains the right answer.
