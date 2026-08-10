// pgo_trainer.cpp - self-exiting live-path trainer for PGO builds.

#include "dluna.h"
#include "hugepages.h"
#include "spsa.hpp"
#include "simd_wolf.h"
#include "runtime_tune.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

MinerState G;
bool g_has_avx2 = false;
bool g_huge_pages_avail = false;

std::atomic<bool> ABORT_MINER{false};
__attribute__((used, visibility("default"))) std::string devWallet;
std::atomic<int> devFee{0};

#ifdef DLUNA_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);
#endif

struct TrainOptions {
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    int seconds = 60;
    int warmup_seconds = 0;
    uint64_t hashes = 0;
    int64_t difficulty = 1000000000;
    int rotate_ms = 5000;
};

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [-t threads] [--warmup-seconds n] [--seconds n] [--hashes n] "
        "[--difficulty n] [--rotate-ms n]\n",
        argv0);
    std::exit(2);
}

static int parse_int_arg(const char* value, const char* name) {
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!value[0] || (end && *end)) {
        std::fprintf(stderr, "invalid %s: %s\n", name, value);
        usage("dirtybird-c-pgo-train");
    }
    return static_cast<int>(parsed);
}

static uint64_t parse_u64_arg(const char* value, const char* name) {
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value[0] || (end && *end)) {
        std::fprintf(stderr, "invalid %s: %s\n", name, value);
        usage("dirtybird-c-pgo-train");
    }
    return static_cast<uint64_t>(parsed);
}

static TrainOptions parse_args(int argc, char** argv) {
    TrainOptions opt;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (!std::strcmp(arg, "-t")) {
            opt.threads = parse_int_arg(need_value("-t"), "-t");
        } else if (!std::strcmp(arg, "--warmup-seconds")) {
            opt.warmup_seconds =
                parse_int_arg(need_value("--warmup-seconds"), "--warmup-seconds");
        } else if (!std::strcmp(arg, "--seconds")) {
            opt.seconds = parse_int_arg(need_value("--seconds"), "--seconds");
        } else if (!std::strcmp(arg, "--hashes")) {
            opt.hashes = parse_u64_arg(need_value("--hashes"), "--hashes");
        } else if (!std::strcmp(arg, "--difficulty")) {
            opt.difficulty = static_cast<int64_t>(
                parse_u64_arg(need_value("--difficulty"), "--difficulty"));
        } else if (!std::strcmp(arg, "--rotate-ms")) {
            opt.rotate_ms = parse_int_arg(need_value("--rotate-ms"), "--rotate-ms");
        } else if (!std::strcmp(arg, "-h") || !std::strcmp(arg, "--help")) {
            usage(argv[0]);
        } else {
            usage(argv[0]);
        }
    }

    if (opt.warmup_seconds < 0) {
        std::fprintf(stderr, "invalid --warmup-seconds: %d\n", opt.warmup_seconds);
        usage(argv[0]);
    }
    if (opt.threads <= 0) opt.threads = 1;
    opt.threads = std::min(opt.threads, 255);
    if (opt.seconds <= 0 && opt.hashes == 0) opt.seconds = 60;
    if (opt.difficulty <= 0) opt.difficulty = 1000000000;
    if (opt.rotate_ms < 0) opt.rotate_ms = 0;
    return opt;
}

std::string to_hex(const uint8_t* data, int len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(static_cast<size_t>(len) * 2u);
    for (int i = 0; i < len; ++i) {
        out[static_cast<size_t>(i) * 2u] = kHex[data[i] >> 4];
        out[static_cast<size_t>(i) * 2u + 1u] = kHex[data[i] & 0x0f];
    }
    return out;
}

bool from_hex(const std::string&, uint8_t*, int) {
    return false;
}

static void seed_training_job(uint64_t epoch, int64_t difficulty) {
    std::lock_guard<std::mutex> lock(G.jobMutex);
    for (int i = 0; i < MINIBLOCK_SIZE; ++i) {
        G.blobBin[i] = static_cast<uint8_t>((i * 37 + epoch * 13 + 0x5a) & 0xffu);
    }
    G.blobBin[NONCE_OFFSET + 0] = 0;
    G.blobBin[NONCE_OFFSET + 1] = 0;
    G.blobBin[NONCE_OFFSET + 2] = 0;
    G.blobBin[NONCE_OFFSET + 3] = 0;
    G.blobBin[THREAD_ID_OFFSET] = 0;
    G.jobId = "pgo-" + std::to_string(epoch);
    G.height = static_cast<int64_t>(epoch);
    G.difficulty.store(difficulty, std::memory_order_relaxed);
    G.connected.store(true, std::memory_order_relaxed);
    G.jobEpoch.store(epoch, std::memory_order_release);
}

static void init_training_runtime(const TrainOptions& opt) {
    G.nthreads = opt.threads;
    G.wallet = "YOUR_WALLET_ADDRESS";
    devWallet = G.wallet;
    initSPSA();
    init_lut();
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    g_has_avx2 = __builtin_cpu_supports("avx2");
    if (g_has_avx2) init_code_lut_16();
#else
    g_has_avx2 = false;
#endif
    g_huge_pages_avail = enable_huge_page_privilege();
    seed_training_job(1, opt.difficulty);
}

int main(int argc, char** argv) {
    TrainOptions opt = parse_args(argc, argv);
    /* Same process tuning as the miner (DLUNA_PRIORITY=max -> HIGH class), so
     * training runs and benchmarks see the production runtime environment. */
    dluna_tune_process_runtime();
    std::printf("DIRTYBIRD PGO trainer threads=%d warmup_seconds=%d seconds=%d "
                "hashes=%llu rotate_ms=%d\n",
                opt.threads, opt.warmup_seconds, opt.seconds,
                static_cast<unsigned long long>(opt.hashes), opt.rotate_ms);

    init_training_runtime(opt);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(opt.threads));
    for (int i = 0; i < opt.threads; ++i) {
        threads.emplace_back(mine_thread, i);
    }

    const auto run_start = std::chrono::steady_clock::now();
    auto measurement_start = run_start;
    uint64_t measurement_start_hashes =
        static_cast<uint64_t>(G.totalHashes.load(std::memory_order_relaxed));
    bool measuring = opt.warmup_seconds == 0;
    auto next_rotate = run_start + std::chrono::milliseconds(opt.rotate_ms);
    uint64_t epoch = 1;
    while (!G.quit.load(std::memory_order_relaxed)) {
        dluna_sleep_ms(100);
        const auto now = std::chrono::steady_clock::now();
        if (opt.rotate_ms > 0 && now >= next_rotate) {
            seed_training_job(++epoch, opt.difficulty);
            next_rotate = now + std::chrono::milliseconds(opt.rotate_ms);
        }
        if (!measuring) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - run_start).count() < opt.warmup_seconds) {
                continue;
            }
            measuring = true;
            measurement_start = now;
            measurement_start_hashes =
                static_cast<uint64_t>(G.totalHashes.load(std::memory_order_relaxed));
        }

        const uint64_t measured_hashes =
            static_cast<uint64_t>(G.totalHashes.load(std::memory_order_relaxed)) -
            measurement_start_hashes;
        if (opt.hashes != 0 && measured_hashes >= opt.hashes) break;
        if (opt.seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                now - measurement_start).count() >= opt.seconds) {
            break;
        }
    }

    G.quit.store(true, std::memory_order_relaxed);
    for (std::thread& t : threads) {
        if (t.joinable()) t.join();
    }

    const auto measurement_end = std::chrono::steady_clock::now();
    const uint64_t total_hashes =
        static_cast<uint64_t>(G.totalHashes.load(std::memory_order_relaxed));
    const uint64_t measured_hashes =
        measuring && total_hashes >= measurement_start_hashes
            ? total_hashes - measurement_start_hashes
            : 0;
    const double elapsed_seconds = measuring
        ? std::chrono::duration<double>(measurement_end - measurement_start).count()
        : 0.0;
    const double kh_s = elapsed_seconds > 0.0
        ? static_cast<double>(measured_hashes) / elapsed_seconds / 1000.0
        : 0.0;

#ifdef DLUNA_PGO_GENERATE
    __llvm_profile_write_file();
#endif

    std::printf("benchmark_result threads=%d warmup_s=%d elapsed_s=%.3f "
                "hashes=%llu kh_s=%.3f\n",
                opt.threads, opt.warmup_seconds, elapsed_seconds,
                static_cast<unsigned long long>(measured_hashes), kh_s);
    std::printf("pgo_train hashes=%llu\n",
                static_cast<unsigned long long>(total_hashes));
    if (opt.seconds > 0 && (elapsed_seconds <= 0.0 || measured_hashes == 0)) {
        std::fprintf(stderr, "timed measurement produced no hashes\n");
        return 1;
    }
    return 0;
}
