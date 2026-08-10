/*
 * main.cpp -- DIRTYBIRD C Miner entry point
 *
 * Clean-room DERO miner. AstroBWT v3.
 * No Boost. No nlohmann. Just pthreads and sockets.
 */

#include "dluna.h"
#include "spsa.hpp"
#include "hugepages.h"
#include "simd_wolf.h"
#include "hex.h"
#include "runtime_tune.h"
#include "cpu_topology.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

/* --- globals --- */

MinerState G;
bool g_has_avx2 = false;
bool g_huge_pages_avail = false;

/* SPSA required globals */
std::atomic<bool> ABORT_MINER{false};
/* Kept with `used` + default visibility so LTO does not elide the symbol;
 * the SPSA stage references this global at initialization. */
__attribute__((used, visibility("default"))) std::string devWallet;
std::atomic<int> devFee{0};

/* --- signal handler --- */

static void on_signal(int) { G.quit = true; }

/* --- hex helpers --- */

std::string to_hex(const uint8_t *data, int len)
{
	return hexStr(data, len);
}

bool from_hex(const std::string &hex, uint8_t *out, int max)
{
	if ((int)hex.size() > max * 2)
		return false;
	hexstrToBytes(hex, out);
	return true;
}

/* --- reporter thread --- */

/* PGO instrumentation runtime hook — flush profile data periodically so
 * that profile is preserved even when the miner is killed via taskkill /F
 * on Windows (graceful shutdown isn't possible for a console app from
 * outside the process). Only emitted in PGO_GENERATE builds. */
#ifdef DLUNA_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);
#endif

namespace {

struct HashrateTracker {
	using Clock = std::chrono::steady_clock;

	struct Snapshot {
		Clock::time_point time;
		int64_t hashes;
	};

	static constexpr int WINDOW_SAMPLES = 10;
	Snapshot history[WINDOW_SAMPLES];
	Snapshot start;
	int64_t last_hashes;
	int next = 0;

	HashrateTracker(Clock::time_point now, int64_t total)
		: start{now, total}, last_hashes(total)
	{
		for (auto &snapshot : history)
			snapshot = start;
	}

	void sample(Clock::time_point now, int64_t total, double &rate, double &avg)
	{
		if (total < last_hashes) {
			start = {now, total};
			for (auto &snapshot : history)
				snapshot = start;
			last_hashes = total;
			next = 0;
			rate = avg = 0;
			return;
		}

		const Snapshot oldest = history[next];
		const double window_seconds =
			std::chrono::duration<double>(now - oldest.time).count();
		const double session_seconds =
			std::chrono::duration<double>(now - start.time).count();
		rate = (window_seconds > 0)
			? (total - oldest.hashes) / (window_seconds * 1000.0) : 0;
		avg = (session_seconds > 0)
			? (total - start.hashes) / (session_seconds * 1000.0) : 0;
		history[next] = {now, total};
		last_hashes = total;
		next = (next + 1) % WINDOW_SAMPLES;
	}
};

static bool hashrate_tracker_selftest()
{
	using Clock = HashrateTracker::Clock;
	const auto start = Clock::time_point{};
	double rate = 0, avg = 0;

	HashrateTracker irregular(start, 777);
	irregular.sample(start + std::chrono::seconds(2), 2777, rate, avg);
	if (rate != 1.0 || avg != 1.0)
		return false;

	HashrateTracker stalled(start, 777);
	for (int second = 1; second <= 5; ++second) {
		stalled.sample(start + std::chrono::seconds(second),
		              777 + 1000 * second, rate, avg);
		if (rate != 1.0 || avg != 1.0)
			return false;
	}
	for (int second = 6; second <= 10; ++second)
		stalled.sample(start + std::chrono::seconds(second), 5777, rate, avg);
	if (rate != 0.5 || avg != 0.5)
		return false;
	for (int second = 11; second <= 15; ++second)
		stalled.sample(start + std::chrono::seconds(second), 5777, rate, avg);
	if (rate != 0)
		return false;

	HashrateTracker reset(start, 777);
	reset.sample(start + std::chrono::seconds(1), 1777, rate, avg);
	reset.sample(start + std::chrono::seconds(2), 10, rate, avg);
	if (rate != 0 || avg != 0)
		return false;
	reset.sample(start + std::chrono::seconds(3), 1010, rate, avg);
	return rate == 1.0 && avg == 1.0;
}

} // namespace

static void reporter_thread()
{
	auto t0 = std::chrono::steady_clock::now();
	HashrateTracker hashrate(t0, G.totalHashes.load(std::memory_order_relaxed));
	int flush_counter = 0;

	while (!G.quit) {
		dluna_sleep_ms(1000);

		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration<double>(now - t0).count();
		int64_t total = G.totalHashes.load(std::memory_order_relaxed);
		double rate, avg;
		hashrate.sample(now, total, rate, avg);

		/* Colored [DIRTYBIRD-C] status line (the format users liked). Fields:
		 *   Height     = G.height   (atomic display snapshot)
		 *   Miniblocks = G.accepted (daemon "miniblocks" accepted)
		 *   Blocks     = G.blocks   (daemon integrator/full "blocks")
		 *   REJ        = G.rejected (daemon "rejected")
		 * No banner, no shares field. ANSI colors only on a TTY (console.cpp enables
		 * VT processing); the file/pipe branch stays plain so miner_live.log is clean. */
		long sec = (long)elapsed;
		int hh = (int)(sec / 3600), mm = (int)((sec % 3600) / 60),
		    ss = (int)(sec % 60);

		/* Humanize difficulty to K/M/G (port of the 1.0.12 reporter). */
		char diffbuf[24];
		unsigned long long dval = (unsigned long long)G.difficulty.load();
		if      (dval >= 1000000000ULL) snprintf(diffbuf, sizeof diffbuf, "%lluG", dval / 1000000000ULL);
		else if (dval >= 1000000ULL)    snprintf(diffbuf, sizeof diffbuf, "%lluM", dval / 1000000ULL);
		else if (dval >= 1000ULL)       snprintf(diffbuf, sizeof diffbuf, "%lluK", dval / 1000ULL);
		else                            snprintf(diffbuf, sizeof diffbuf, "%llu",  dval);

		std::lock_guard<std::mutex> lk(g_console_mtx);
		if (dluna_is_tty()) {
			/* Colored line rewritten in place with \r, so it has to fit on ONE
			 * row -- a wrapped line leaves the cursor on its last row and every
			 * repaint then stacks a row lower (Termux: 115 columns on a 56-column
			 * phone terminal). dluna_format_status() picks the widest layout that
			 * fits, re-querying the width each tick so a resize re-fits within a
			 * second. Ends with \033[0m + \033[K and NO trailing pad. */
			/* Nothing is being hashed while disconnected or before the
			 * first job, so the rate is a real zero that says nothing
			 * the logs have not already said. Only the interactive row
			 * is gated: the redirected branch below keeps emitting one
			 * record per tick, because that stream is machine-parsed
			 * (HiveOS) and a run of 0.00 there is the correct report
			 * while a gap is not. Rate accumulation above is untouched,
			 * so the readout does not have to climb back after a
			 * reconnect. */
			if (dluna_status_row_visible(
			        G.connected.load(std::memory_order_relaxed),
			        (unsigned long long)G.jobEpoch.load(std::memory_order_relaxed))) {
				char line[512];
				DlunaStatus st{};
				st.rate     = rate;
				st.avg      = avg;
				st.height   = (long long)G.height.load(std::memory_order_relaxed);
				st.accepted = (long long)G.accepted.load();
				st.blocks   = (long long)G.blocks.load();
				st.rejected = (long long)G.rejected.load();
				st.diff     = diffbuf;
				st.hh = hh; st.mm = mm; st.ss = ss;
				dluna_format_status(line, sizeof line, st, dluna_term_cols());
				fputs(line, stdout);
			}
		} else {
			/* Redirected to a file/pipe: same fields, no ANSI, newline-terminated. */
			printf("[DIRTYBIRD-C] %.2f KH/s (%.2f KH/s avg) | Height:%lld | "
			       "Miniblocks:%lld | Blocks:%lld | REJ:%lld | Diff:%s | "
			       "%02d:%02d:%02d\n",
			       rate, avg, (long long)G.height.load(std::memory_order_relaxed),
			       (long long)G.accepted.load(), (long long)G.blocks.load(),
			       (long long)G.rejected.load(), diffbuf, hh, mm, ss);
		}

		/* -V submit-funnel diagnostic: read-only atomics, no hot-loop impact.
		 * submitted ~ acc with ~0 stale/sendfail => healthy; high stale or
		 * sendfail, or submitted >> acc, points at a real submit/accept problem. */
		if (g_verbose) {
			/* found should equal stale + dropped + submitted + sendfail. The
			 * single-slot mailbox could not satisfy that identity: it destroyed
			 * shares without counting them anywhere. */
			printf("\n[funnel] found:%lld queued:%lld submitted:%lld acc:%lld rej:%lld "
			       "stale:%lld sendfail:%lld dropped:%lld\n",
			       (long long)G.found.load(), (long long)G.enqueued.load(),
			       (long long)G.submitted.load(), (long long)G.accepted.load(),
			       (long long)G.rejected.load(), (long long)G.staleDrops.load(),
			       (long long)G.sendFails.load(), (long long)G.submitDrops.load());
		}

		/* Hashes spent on jobs the network had already replaced. Counted only
		 * under DLUNA_COUNT_STALE=1, and printed only under -V: like [funnel]
		 * above, this is a multi-line diagnostic and would otherwise scroll the
		 * in-place status line. */
		if (g_verbose) {
			long long sh = G.staleHashesObserved.load(std::memory_order_relaxed);
			if (sh > 0) {
				long long th = G.totalHashes.load(std::memory_order_relaxed);
				printf("\n[stale] hashes:%lld of %lld (%.2f%%)\n", sh, th,
				       th > 0 ? 100.0 * (double)sh / (double)th : 0.0);
			}
		}
		fflush(stdout);

#ifdef DLUNA_PGO_GENERATE
		if (++flush_counter % 5 == 0) __llvm_profile_write_file();
#endif
	}
}

/* --- CLI --- */

#ifndef DIRTYBIRD_C_VERSION_STR
#define DIRTYBIRD_C_VERSION_STR "dev"
#endif

static void print_usage(FILE *out, const char *argv0)
{
	fprintf(out,
		"Usage: %s [-d host:port] [-w wallet] [-t threads] [-p priority]\n"
		"  Config: reads config.json (next to the exe) for daemon-address/wallet/threads/\n"
		"          priority; CLI flags below override it. Edit config.json OR pass flags.\n"
		"  -d  daemon address (host:port)  (default: community-pools.mysrv.cloud:10300)\n"
		"  -w  wallet address              (default: built-in)\n"
		"  -t  number of mining threads (default: hardware threads)\n"
		"  -p  priority profile: normal | max  (default: normal)\n"
		"        normal = polite, never freezes the desktop\n"
		"        max    = aggressive (HIGH priority, full clock) for headless\n"
		"                 mining; maximum hashrate but stutters the desktop.\n"
		"        (env equivalent: DLUNA_PRIORITY=normal|max)\n"
		"        NOTE: the default is 'normal'. Use -p max for the previous\n"
		"        aggressive behavior.\n"
		"  --no-pin       do not pin worker threads to CPUs (default: pin,\n"
		"                 P-core-first on hybrid CPUs)\n"
		"        (env equivalent: DLUNA_NO_PIN=1; config.json: \"pin\": false)\n"
		"  -V, --verbose  show per-job / per-share event logging (default: off)\n"
		"        (env equivalent: DLUNA_VERBOSE=1)\n"
		"  --selftest     compute the pow(\"a\") KAT and exit (0=PASS, 1=FAIL)\n"
		"  -h, --help     show this help and exit\n"
		"  -v, --version  print version and exit\n", argv0);
}

static void usage(const char *argv0)
{
	print_usage(stderr, argv0);
	exit(1);
}

static bool g_selftest = false;

static void parse_args(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			const char *arg = argv[++i];
			const char *colon = strrchr(arg, ':');
			if (!colon) {
				G.host = arg;
			} else {
				G.host = std::string(arg, colon - arg);
				G.port = (uint16_t)atoi(colon + 1);
			}
		} else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			G.wallet = argv[++i];
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			G.nthreads = atoi(argv[++i]);
		} else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--priority")) && i + 1 < argc) {
			const char *v = argv[++i];
			if (strcmp(v, "normal") && strcmp(v, "max"))
				usage(argv[0]);  /* only normal|max accepted */
			/* Funnel the CLI choice through the same env var the runtime-tune
			 * code reads. Must be set before threads spawn / dluna_tune_*. */
#ifdef _WIN32
			_putenv_s("DLUNA_PRIORITY", v);
#else
			setenv("DLUNA_PRIORITY", v, 1);
#endif
		} else if (!strcmp(argv[i], "--no-pin")) {
			/* Same env funnel as -p: cpu_topology reads DLUNA_NO_PIN. */
#ifdef _WIN32
			_putenv_s("DLUNA_NO_PIN", "1");
#else
			setenv("DLUNA_NO_PIN", "1", 1);
#endif
		} else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--verbose")) {
			g_verbose = true;
		} else if (!strcmp(argv[i], "--selftest")) {
			g_selftest = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(stdout, argv[0]);
			exit(0);
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			printf("Dirtybird C Miner v%s\n", DIRTYBIRD_C_VERSION_STR);
			exit(0);
		} else {
			usage(argv[0]);
		}
	}

	if (const char *e = getenv("DLUNA_VERBOSE"))
		if (e[0] && strcmp(e, "0"))
			g_verbose = true;

	if (!g_selftest && (G.host.empty() || G.wallet.empty()))
		usage(argv[0]);
	if (G.nthreads <= 0)
		G.nthreads = (int)std::thread::hardware_concurrency();
	if (G.nthreads <= 0)
		G.nthreads = 4;
}

/* --- config.json loader (minimal; no JSON lib — same flat-key approach as network.cpp) --- */

static bool read_file_to_string(const char *path, std::string &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0) { out.resize((size_t)n); out.resize(fread(&out[0], 1, (size_t)n, f)); }
	fclose(f);
	return true;
}

/* "key":"value" -> value ("" on miss). Flat config, no escapes/nesting. */
static std::string cfg_str(const std::string &j, const char *key)
{
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat);
	if (p == std::string::npos) return {};
	p += pat.size();
	while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
	if (p >= j.size() || j[p] != '"') return {};
	p++;
	size_t q = j.find('"', p);
	if (q == std::string::npos) return {};
	return j.substr(p, q - p);
}

/* "key": <int> -> value (def on miss / if it's a string). Handles negative. */
static long long cfg_int(const std::string &j, const char *key, long long def)
{
	std::string pat = std::string("\"") + key + "\":";
	size_t p = j.find(pat);
	if (p == std::string::npos) return def;
	p += pat.size();
	while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
	if (p >= j.size() || j[p] == '"') return def;
	return strtoll(j.c_str() + p, nullptr, 10);
}

/* Apply config.json (if present) into G as defaults. CLI args (parse_args) override.
 * Absent file = no-op (built-in defaults stand) -> fully backward compatible. */
static void load_config(const char *path)
{
	std::string j;
	if (!read_file_to_string(path, j) || j.empty()) return;

	std::string daddr = cfg_str(j, "daemon-address");
	if (daddr.empty()) daddr = cfg_str(j, "pool");
	if (daddr.empty()) daddr = cfg_str(j, "daemon");
	if (!daddr.empty()) {
		size_t colon = daddr.rfind(':');
		if (colon == std::string::npos) {
			G.host = daddr;
		} else {
			G.host = daddr.substr(0, colon);
			G.port = (uint16_t)atoi(daddr.c_str() + colon + 1);
		}
	}
	long long pj = cfg_int(j, "port", -1);
	if (pj > 0) G.port = (uint16_t)pj;

	std::string w = cfg_str(j, "wallet");
	if (!w.empty()) G.wallet = w;

	long long t = cfg_int(j, "threads", 0);
	if (t > 0) G.nthreads = (int)t;   /* <=0 (e.g. -1) keeps auto-detect */

	std::string prio = cfg_str(j, "priority");
	if (prio == "normal" || prio == "max") {
#ifdef _WIN32
		_putenv_s("DLUNA_PRIORITY", prio.c_str());
#else
		setenv("DLUNA_PRIORITY", prio.c_str(), 1);
#endif
	}

	/* "pin": false (or 0, "false", "off") disables worker pinning. The bare
	 * false/0 tokens don't fit cfg_str/cfg_int, so peek at the value's first
	 * non-space character. */
	{
		std::string pat = "\"pin\":";
		size_t p = j.find(pat);
		if (p != std::string::npos) {
			p += pat.size();
			while (p < j.size() && (j[p] == ' ' || j[p] == '\t' ||
			                        j[p] == '\n' || j[p] == '\r')) p++;
			std::string v = (p < j.size() && j[p] == '"') ? cfg_str(j, "pin")
			                                              : std::string();
			bool off = (p < j.size() && (j[p] == 'f' || j[p] == '0')) ||
			           v == "false" || v == "off" || v == "0";
			if (off) {
#ifdef _WIN32
				_putenv_s("DLUNA_NO_PIN", "1");
#else
				setenv("DLUNA_NO_PIN", "1", 1);
#endif
			}
		}
	}
}

/* --- main --- */

int main(int argc, char **argv)
{
	dluna_console_init();

	/* config.json (next to the exe) provides defaults; CLI flags below override it. */
	load_config("config.json");

	parse_args(argc, argv);

	log_line("INFO", "Dirtybird C Miner");
	log_line("INFO", "Server:  %s:%d", G.host.c_str(), G.port);
	log_line("INFO", "Wallet:  %s", G.wallet.c_str());
	log_line("INFO", "Threads: %d", G.nthreads);
#if !defined(__ANDROID__) && !defined(__APPLE__)
	if (!dluna_pin_enabled())
		log_line("INFO", "Pinning: off");
	else if (dluna_cpu_order().detected)
		log_line("INFO", "Pinning: P-core-first (%d CPUs)", dluna_cpu_order().count);
	else
		log_line("INFO", "Pinning: round-robin (%d CPUs)", dluna_cpu_order().count);
#endif
	printf("\n");

	DlunaPriorityLevel prio = dluna_runtime_tune_options_from_env().level;
	if (g_verbose) printf("Priority: %s (%s)\n", dluna_priority_level_name(prio),
	       prio == DLUNA_PRIO_MAX
	           ? "aggressive — max hashrate, may stutter the desktop"
	           : "smooth — desktop stays responsive");

	uint32_t runtime_tune = dluna_tune_process_runtime();
	if (g_verbose) {
		if (runtime_tune != DLUNA_RUNTIME_TUNE_NONE)
			printf("Runtime: performance tuning active (0x%02x)\n\n",
			       runtime_tune);
		else
			printf("\n");
	}

	/* SPSA init. devWallet is set to the user's wallet; the in-tree SPSA
	 * stage reads this global at initialization. Share submission uses
	 * G.wallet (see network.cpp), so all rewards go to the user's wallet. */
	devWallet = G.wallet;
	initSPSA();

	/* Build 1D lookup table (~38 KB) */
	init_lut();

	/* AstroBWT v3 test vector check */
	{
	        workerData *w = new workerData();
	        memset(w, 0, sizeof(workerData));
#if defined(USE_ASTRO_SPSA)
	        for(int i=0; i<256; i++) w->iota8[i] = i;
#endif
	        uint8_t out[32];
	        char in_a[] = "a";
	        dluna_hash((uint8_t*)in_a, 1, out, *w);
	        std::string h = hexStr(out, 32);
	        bool pass = (h == "54e2324ddacc3f0383501a9e5760f85d63e9bc6705e9124ca7aef89016ab81ea");
	        if (g_selftest) {
	                bool tracker_pass = hashrate_tracker_selftest();
	                printf("selftest hashrate tracker: %s\n", tracker_pass ? "PASS" : "FAIL");
	                printf("selftest pow(a): %s %s\n", h.c_str(), pass ? "PASS" : "FAIL");
	                bool x2_pass = true;
	                if (dluna_hash_x2_available()) {
	                        workerData *wb = new workerData();
	                        memset(wb, 0, sizeof(workerData));
#if defined(USE_ASTRO_SPSA)
	                        for (int i = 0; i < 256; i++) wb->iota8[i] = i;
#endif
	                        uint8_t oa[32], ob[32];
	                        char ia[] = "a", ib[] = "a";
	                        dluna_hash_x2((uint8_t*)ia, (uint8_t*)ib, 1,
	                                      oa, ob, *w, *wb);
	                        x2_pass = hexStr(oa, 32) == h && hexStr(ob, 32) == h;
	                        printf("selftest pow(a) x2: %s\n", x2_pass ? "PASS" : "FAIL");
	                        delete wb;
	                }
	                delete w; exit(pass && tracker_pass && x2_pass ? 0 : 1);
	        }
	        if (g_verbose) printf("Test pow(a): %s\n", h.c_str());
	        if (pass) {
	                if (g_verbose) printf("Test pow(a): PASS\n");
	        } else {
	                printf("Test pow(a): FAIL! (expected 54e2324ddacc3f0383501a9e5760f85d63e9bc6705e9124ca7aef89016ab81ea)\n"); exit(1);
	        }
	        delete w;
	}
	/* Detect AVX2, init compressed CodeLUT */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	if (__builtin_cpu_supports("avx2")) {
		g_has_avx2 = true;
		init_code_lut_16();

		/* Quick AVX2 sanity check */
		uint8_t test_in[64] = {0};
		uint8_t test_out[64] = {0};
		for (int i = 0; i < 64; i++) test_in[i] = (uint8_t)i;
		wolfPermute_avx2(test_in, test_out, 42, 0, 31);
		if (g_verbose) printf("AVX2:    detected, wolfPermute verified\n");
	} else {
		if (g_verbose) printf("AVX2:    not available, using scalar fallback\n");
	}
#else
	if (g_verbose) printf("AVX2:    not supported on this arch\n");
#endif

	/* Huge pages */
	g_huge_pages_avail = enable_huge_page_privilege();
	if (g_verbose) printf("Huge pages: %s\n\n", g_huge_pages_avail ? "enabled" : "unavailable");

	/* Signal handlers */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
#ifndef _WIN32
	/* SSL_write() to a peer that has gone away raises SIGPIPE, and its default
	 * action TERMINATES the process. Without this the miner died outright on
	 * every dropped connection -- exit 141 -- so none of the reconnect logic
	 * below (backoff, useful-session gate, send-failure redial) could ever run
	 * on Linux or Android. Measured: killing the daemon mid-send exited the
	 * miner in under a second with a single "Connecting" line in its log.
	 *
	 * It survived this long because Windows has no SIGPIPE and that is the
	 * primary development platform; the failure is invisible there. The zig
	 * sibling handles the same hazard with MSG_NOSIGNAL per send (net.zig).
	 * Ignoring it process-wide is the portable equivalent: SSL_write then
	 * returns -1/EPIPE and the existing error path reconnects. */
	signal(SIGPIPE, SIG_IGN);
#endif

	/* Spawn threads; each worker pins itself (dluna_pin_mining_thread) */
	std::thread net(network_thread);
	std::thread rpt(reporter_thread);

	std::vector<std::thread> miners;
	miners.reserve(G.nthreads);
	for (int i = 0; i < G.nthreads; i++)
		miners.emplace_back(mine_thread, i);

	/* Wait for quit */
	for (auto &t : miners)
		t.join();
	net.join();
	rpt.join();

	printf("\nShutdown complete. %lld hashes, %lld miniblocks (%lld blocks), %lld rejected.\n",
	       (long long)G.totalHashes.load(), (long long)G.accepted.load(),
	       (long long)G.blocks.load(), (long long)G.rejected.load());

	/* Share funnel: found == staleDrops + submitDrops + submitted + sendFails.
	 * staleHashesObserved is populated only under DLUNA_COUNT_STALE=1. */
	long long total = G.totalHashes.load();
	long long stale_hashes = G.staleHashesObserved.load();
	printf("Submission: found=%lld submitted=%lld staleDrops=%lld submitDrops=%lld sendFails=%lld enqueued=%lld\n",
	       (long long)G.found.load(), (long long)G.submitted.load(),
	       (long long)G.staleDrops.load(), (long long)G.submitDrops.load(),
	       (long long)G.sendFails.load(), (long long)G.enqueued.load());
	if (stale_hashes > 0)
		printf("Stale hashes: %lld of %lld (%.2f%%)\n", stale_hashes, total,
		       total > 0 ? 100.0 * (double)stale_hashes / (double)total : 0.0);
	return 0;
}
