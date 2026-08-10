# DIRTYBIRD C Miner

High-performance AstroBWTv3 CPU miner for DERO. Clean-room C++ (no Boost; pthreads + sockets),
descriptor-SA pipeline with optional profile-guided optimization (PGO).

**Releases:** [latest release](https://github.com/Dirtybird99/Dirtybird-C-Miner/releases/latest)
· x86-64 (AVX2) and aarch64 (ARMv8) · Windows, Linux (amd64 + arm64), macOS (Apple Silicon), HiveOS/MMPOS, Android.

## Build

Toolchain: clang + lld, CMake, OpenSSL. On Windows use MSYS2 MINGW64.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target dirtybird-c-miner
```

For the fastest binary, use the two-pass PGO flow documented in `build.sh` / `CMakeLists.txt`.

## Usage

Configure via **`config.json`** (next to the exe) **or** CLI flags — your choice. Precedence is
**built-in defaults → `config.json` → CLI flags** (CLI wins). Editing `config.json` is enough; the
bundled `start.bat`/`start.sh` runs the miner with no flags so it picks up `config.json`.

```jsonc
// config.json
{
  "daemon-address": "host:port",
  "wallet": "dero1...",
  "threads": -1,          // -1 = auto (all hardware threads)
  "priority": "normal"    // normal (desktop-safe) | max (headless)
}
```

```
dirtybird-c-miner [-d <host:port>] [-w <wallet>] [-t <threads>] [-p normal|max]
```

| flag | meaning |
|------|---------|
| `-d` | daemon address `host:port` (overrides `daemon-address`) |
| `-w` | DERO wallet address (overrides `wallet`) |
| `-t` | mining threads (overrides `threads`) |
| `-p` | priority profile: `normal` (default, desktop-safe) or `max` (headless) |
| `--no-pin` | disable CPU pinning (default: pin workers, P-core-first on hybrid CPUs; also `"pin": false` in config.json or `DLUNA_NO_PIN=1`) |
| `-h`, `--help` / `-v`, `--version` | help / version |

On CPUs with SHA-NI each worker mines two nonces at a time and finishes them
on interleaved SHA-256 streams; set `DLUNA_NO_SHA_X2=1` to disable.

Workers check for a new job every nonce, so work the daemon has replaced is
abandoned within one hash. `DLUNA_JOB_POLL=<n>` loosens that to every `n`
nonces (rounded up to a power of two) for comparison; `0` and `1` both mean
every nonce. `DLUNA_COUNT_STALE=1` adds a `[stale]` line under `-V` reporting
what share of hashes went to already-replaced jobs. A `Submission:` funnel
line is printed at shutdown.

## Correctness

At startup the miner computes `pow("a")` and verifies it equals
`54e2324ddacc3f0383501a9e5760f85d63e9bc6705e9124ca7aef89016ab81ea`.

## Performance

~21 KH/s sustained at 20 threads on an i7-13700HX (2-hour live average 20.8,
flat). Measure over >=10 minutes for a representative sustained figure.

## Android / Termux (mobile)

A download-only setup script handles everything: it fetches the pre-built aarch64
Android release, writes `config.json`, acquires a wake-lock (so Android Doze doesn't
pause the miner), and runs with auto-restart.

It offers a menu of daemons and pools, or a custom `host:port`. Take the pool
(option 1, the default) on a phone. The solo nodes pay out at network difficulty,
which at phone hashrates means hours between rewards — the counters sit at zero long
enough to look broken, even though nothing is wrong.

```bash
curl -fsSL https://raw.githubusercontent.com/Dirtybird99/Dirtybird-C-Miner/master/scripts/termux-setup.sh | bash
```

Or run it directly from a checkout:

```bash
bash scripts/termux-setup.sh              # install + run
bash scripts/termux-setup.sh --update     # upgrade to latest release
bash scripts/termux-setup.sh --reconfigure  # re-prompt for pool/wallet/threads
bash scripts/termux-setup.sh --uninstall  # remove installed files
```

Requirements: Termux (aarch64 / 64-bit ARM only). Install `termux-api` in Termux for
wake-lock support. On Android, `-p max` is currently a no-op because priority tuning
is Windows-only. Use the installer thread prompt (or `-t`) to balance hashrate,
temperature, and battery use.

## License

MIT - see [LICENSE](LICENSE).
