/*
 * dluna.h -- DIRTYBIRD C Miner master header
 *
 * The entire miner's public interface in one file.
 * No forward declarations scattered across 12 headers.
 * If you can't understand this file, you can't understand the miner.
 */
#pragma once

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <cstdint>
#include <cstring>
#include <deque>

#include <openssl/sha.h>

/* Platform sleep -- no nanosleep64 link errors on TDM-GCC. */
#ifdef _WIN32
#include <windows.h>
static inline void dluna_sleep_ms(int ms) { Sleep(ms); }
#else
#include <poll.h>
/* poll(nullptr, 0, ms) sleeps ms milliseconds without touching the nanosleep
 * symbol. A legacy -Wl,--wrap=nanosleep (DRM neutraliser for the proprietary
 * AstroSPSA library, since removed) turned std::this_thread::sleep_for into a
 * POSIX no-op: reporter busy-spin, 0.00 KH/s display, instant reconnect
 * backoff. Test sleep_ms pins the contract that this really sleeps.
 * ms <= 0 returns immediately -- poll(-1) would block forever. */
static inline void dluna_sleep_ms(int ms) {
    if (ms > 0) ::poll(nullptr, 0, ms);
}
#endif

#include "astroworker.h"

/*
 * DERO protocol constants.
 * These are defined by the network. Change them and your shares get rejected.
 */
enum {
    MINIBLOCK_SIZE    = 48,
    NONCE_OFFSET      = 43,  /* bytes 43-46, 4 bytes big-endian */
    THREAD_ID_OFFSET  = 47,
    HASH_SIZE         = 32,
};

/*
 * MinerState -- all shared mutable state lives here.
 *
 * One instance. Global. If you need a second one, you've made
 * a design mistake.
 */
struct MinerState {
    /* Job state (jobMutex protects blob/jobId on writes) */
    std::atomic<uint64_t> jobEpoch{0};
    std::atomic<uint64_t> difficulty{0};
    std::atomic<bool>     connected{false};
    std::atomic<bool>     quit{false};

    std::string blob;           /* hex, 96 chars */
    uint8_t     blobBin[48];    /* decoded binary */
    std::string jobId;
    std::atomic<int64_t> height{0};
    std::mutex  jobMutex;
    std::condition_variable newJob;

    /* Share submission -- bounded FIFO queue (submitMutex guards submitQueue).
     * Replaces the single-slot mailbox, which silently DESTROYED a found share
     * when a second thread found one before the network thread drained it. With
     * 20+ worker threads two near-simultaneous finds must both reach the wire. */
    struct PendingSubmit {
        std::string jobId;
        std::string blobHex;
        uint64_t    epoch;
    };
    static constexpr size_t SUBMIT_QUEUE_MAX = 32; /* safety valve; overflow is pathological */
    std::deque<PendingSubmit> submitQueue;         /* under submitMutex */
    std::mutex  submitMutex;

    /* Counters */
    std::atomic<int64_t> totalHashes{0};
    std::atomic<int64_t> accepted{0};
    std::atomic<int64_t> rejected{0};
    std::atomic<int64_t> blocks{0};
    std::atomic<int64_t> submitted{0};
    std::atomic<int64_t> sendFails{0};
    std::atomic<int64_t> staleDrops{0};  /* gate1 + gate2 (pre-enqueue) and per-item at drain */
    /* Shares lost to queue pressure: the oldest entry evicted because the queue
     * was already at SUBMIT_QUEUE_MAX. Under the old single-slot mailbox this
     * fired on every collision; with a 32-deep queue it should never fire at
     * all, which is exactly what makes it useful -- a non-zero value now means
     * the network thread is not draining, not that the mailbox was too small.
     * Keeps its name from the depth-1 era so the -V field stays stable. */
    std::atomic<int64_t> submitDrops{0};
    /* The funnel identity, which the single-slot mailbox could not satisfy:
     *   found == staleDrops + submitDrops + submitted + sendFails
     * `found` counts every discovery BEFORE any gate; `enqueued` counts those
     * that survived both pre-enqueue stale gates. */
    std::atomic<int64_t> found{0};
    std::atomic<int64_t> enqueued{0};
    /* Hashes computed against an already-advanced epoch. Only counted when
     * DLUNA_COUNT_STALE=1 -- measurement for job-poll cadence tuning. */
    std::atomic<int64_t> staleHashesObserved{0};

    /* Config -- set once at startup, read-only after.
     * Defaults let the binary run with no args; override via -d/-w. */
    std::string host{"community-pools.mysrv.cloud"};
    uint16_t    port{10300};
    std::string wallet{"dero1qyvuemd6z0uzsx5ufc99f0jhyzvvpysmrd2t3526ht7a9dfh7jve2qqt0vu5y"};
    int         nthreads{0};   /* 0 / -1 in config = auto (all hardware threads) */
};

extern MinerState G;
extern bool       g_has_avx2;

/* Queue a found share for the network thread. THE CALLER MUST HOLD
 * G.submitMutex.
 *
 * Returns false only when the queue was already full and the OLDEST entry had
 * to be evicted to make room. Dropping the oldest is deliberate: it is the
 * entry nearest the stale boundary, so it is the one most likely to be rejected
 * anyway, and a fresher share is worth more than an older one.
 *
 * This replaces a single-slot mailbox that overwrote unconditionally, so any
 * second find arriving before the network thread drained destroyed the first.
 * Depth 32 puts that beyond reach for a worker count in the tens: overflow now
 * means the network thread has stopped draining, which is a different and
 * louder problem than a mailbox being one deep.
 *
 * Lock is held only for the push -- no I/O underneath it, so 20+ concurrent
 * enqueuers do not serialise on the socket. Split out so it is unit-testable
 * without a live miner. */
static inline bool dluna_submit_enqueue(MinerState &st, const std::string &jobId,
                                        std::string blobHex, uint64_t jobEpoch)
{
	bool evicted = false;
	if (st.submitQueue.size() >= MinerState::SUBMIT_QUEUE_MAX) {
		st.submitQueue.pop_front();
		st.submitDrops.fetch_add(1, std::memory_order_relaxed);
		evicted = true;
	}
	st.submitQueue.push_back({jobId, std::move(blobHex), jobEpoch});
	st.enqueued.fetch_add(1, std::memory_order_relaxed);
	return !evicted;
}

/* Console output coordination.
 *   g_console_mtx -- serializes every write to the console so timestamped
 *                    event lines never corrupt the in-place status line.
 *   g_verbose     -- -V / DLUNA_VERBOSE: restores per-job/share event logging.
 *   log_line()    -- timestamped, mutex-guarded log ("DD/MM HH:MM:SS.mmm  LEVEL  msg").
 *   dluna_console_init() -- enable VT processing + UTF-8, detect TTY (call once). */
extern std::mutex g_console_mtx;
extern bool       g_verbose;
void log_line(const char *level, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;
void dluna_console_init(void);
bool dluna_is_tty(void);       /* stdout is an interactive VT-capable console */
const char *dluna_clr_eol(void); /* ANSI erase-to-EOL, or "" when unavailable */

/* Visible width of the console in columns. 0 means stdout is NOT a console (a
 * file or pipe); a console whose size cannot be queried reports an assumed 80
 * rather than 0, because dluna_format_status() reads 0 as an unbounded width
 * budget and would let the line wrap. Cheap enough to call once per reporter
 * tick, which is how a terminal resize is picked up without a SIGWINCH handler. */
int dluna_term_cols(void);

/* One reporter tick's worth of numbers, ready to render. */
struct DlunaStatus {
	double      rate;      /* trailing 10-sample KH/s */
	double      avg;       /* cumulative KH/s */
	long long   height;
	long long   accepted;  /* daemon "miniblocks" */
	long long   blocks;    /* daemon "blocks" */
	long long   rejected;
	const char *diff;      /* already humanized, e.g. "795K" */
	int         hh, mm, ss;
};

/* Renders the in-place status line into `out` (leading \r, no trailing newline,
 * always closed with reset + erase-to-EOL). `cols` is the terminal width from
 * dluna_term_cols(); the widest layout whose VISIBLE width fits `cols - 1` wins,
 * so the line never wraps and \r always rewinds to its own first column. Pass
 * cols <= 0 for "unknown" -- that renders the full layout unconditionally.
 * Does no I/O and reads no miner state (only dluna_clr_eol() for the trailing
 * erase), so it is directly unit-testable. Returns the number of bytes written
 * (excluding the NUL). */
size_t dluna_format_status(char *out, size_t cap, const DlunaStatus &s, int cols);

/* Whether the live status row has anything true to report this tick.
 *
 * A displayed 0.00 KH/s is not a rare fault -- it is what every launch prints.
 * getwork sends no job at connect time (the first arrives on a dispatch tick
 * ~500ms later), so dial + TLS + upgrade + first job is ~1-3s of genuine zero
 * and the first tick at t=1s lands inside it. Reconnect is the same story.
 *
 * No DERO miner in the ecosystem displays a live zero. tnn-miner suppresses its
 * row two ways ("if (!isConnected) return 1;" in src/core/reporter.cpp:31, plus
 * a first-hashrate gate commented "Mining hasn't started yet - don't print
 * status, just accumulate stats"); 8lecramm/dero-c-miner calls print_status
 * only from inside the worker thread, after the job check; DEROFDN/netrunner's
 * GUI shows a grey "---" placeholder and a separate "Offline" label.
 *
 * Suppressing beats naming the reason because the transitions are already
 * logged, and log_line() emits \r + erase first -- so a log record wipes any
 * row already on screen instead of leaving a stale one. Pure, so it is directly
 * unit-testable alongside dluna_format_status(). */
static inline bool dluna_status_row_visible(bool connected, unsigned long long jobEpoch)
{
	return connected && jobEpoch > 0;
}

/* Thread entry points */
void mine_thread(int tid);
void network_thread(void);

/* Difficulty: target = 2^256 / diff */
void compute_target(int64_t diff, uint8_t target[32]);
bool check_hash(const uint8_t hash[32], const uint8_t target[32]);

/* AstroBWT v3 hash */
void dluna_hash(uint8_t *input, int len, uint8_t *output, workerData &w);

/* Two-nonce variant: runs both workers' pipelines through the SA stage
 * serially, then batches the two final SHA-256 passes on interleaved SHA-NI
 * streams. Byte-identical to two dluna_hash calls. Callers must check
 * dluna_hash_x2_available() first; where it is false, dluna_hash_x2 simply
 * performs two sequential hashes with no batching benefit. */
bool dluna_hash_x2_available(void);
void dluna_hash_x2(uint8_t *input_a, uint8_t *input_b, int len,
                   uint8_t *output_a, uint8_t *output_b,
                   workerData &wa, workerData &wb);
void init_lut(void);

/* SHA-256 via SHA-NI linker wraps on x86 or OpenSSL's ARM dispatcher. */
inline void hashSHA256(const uint8_t *in, uint8_t *out, int len) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, in, len);
    SHA256_Final(out, &ctx);
}

/* Hex helpers */
std::string to_hex(const uint8_t *data, int len);
bool from_hex(const std::string &hex, uint8_t *out, int max);
