/*
 * astrobwt.cpp -- AstroBWT v3 hash function (DIRTYBIRD C Miner)
 *
 * Pipeline: Input(48B) -> SHA256 -> Salsa20(expand 32B key to 256B)
 *           -> RC4(KSA+PRGA 256B) -> FNV-1a -> wolfCompute(278 iter)
 *           -> SPSA/libsais -> SHA256 -> OutputHash(32B)
 *
 * Hot path. Every cycle counts.
 */

#include "dluna.h"
#include "dluna_v114.h"
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include "sha256_x2.h"
#endif
#include "spsa.hpp"
#include "simd_wolf.h"
#include "fnv1a.h"
#include "xxhash64.h"
#include "highwayhash/sip_hash.h"
#include <openssl/sha.h>
#include <openssl/rc4.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#include "libsais.h"
}

/* ---- externs from branch.cpp ---- */
extern uint32_t CodeLUT[256];
extern bool g_has_avx2;

/* ---- compressed CodeLUT: 4 nibbles per opcode ---- */
uint16_t CodeLUT_16[256];

void init_code_lut_16()
{
	for (int op = 0; op < 256; op++) {
		uint32_t f = CodeLUT[op];
		uint16_t c = 0;
		for (int i = 3; i >= 0; --i)
			c |= (uint16_t)((f >> (i * 8)) & 0x0F) << (i * 4);
		CodeLUT_16[op] = c;
	}
}

/* ---- branchedOps: 104 opcodes that need pos2val ---- */
static const std::vector<unsigned char> branchedOps_global = {
	1,3,5,9,11,13,15,17,20,21,23,27,29,30,35,39,40,43,45,47,51,54,58,60,62,
	64,68,70,72,74,75,80,82,85,91,92,93,94,103,108,109,115,116,117,119,120,
	123,124,127,132,133,134,136,138,140,142,143,146,148,149,150,154,155,159,
	161,165,168,169,176,177,178,180,182,184,187,189,190,193,194,195,199,202,
	203,204,212,214,215,216,219,221,222,223,226,227,230,231,234,236,239,240,
	241,242,250,253
};

/* 2026-05-01 stage-1 replay wrapper accessor.
 * Extracted from dluna_hash's inline SHA256->Salsa20 expansion so the replay
 * harness can call the same transformation in isolation. */
void dluna_internal_salsa20_expand(const uint8_t* key32, uint8_t* out256)
{
	ucstk::Salsa20 salsa20;
	uint8_t zero[256] = {0};
	salsa20.setKey(key32);
	salsa20.setIv(zero);
	salsa20.processBytes(zero, out256, 256);
}

/* ---- 1D lookup table globals ---- */
static uint8_t* lookup1D;
static uint8_t g_is_branched[256];
static uint8_t g_reg_idx[256];
static bool g_lut_ready;

/* ---- helpers ---- */

static inline uint8_t rl8(uint8_t x, unsigned d)
{
	d &= 7;
	if (d == 0) return x;
	return (uint8_t)((x << d) | (x >> (8 - d)));
}

static inline uint8_t reverse8(uint8_t b)
{
	b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
	b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
	b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
	return b;
}

/* ---- scalar wolfBranch: 16 micro-ops via switch ---- */

static inline uint8_t wolfBranch(uint8_t val, uint8_t pos2val, uint32_t opcode)
{
	for (int i = 3; i >= 0; --i) {
		uint8_t insn = (opcode >> (i << 3)) & 0xFF;
		switch (insn) {
		case 0:  val += val; break;
		case 1:  val -= (val ^ 97); break;
		case 2:  val *= val; break;
		case 3:  val ^= pos2val; break;
		case 4:  val = ~val; break;
		case 5:  val &= pos2val; break;
		case 6:  val <<= (val & 3); break;
		case 7:  val >>= (val & 3); break;
		case 8:  val = reverse8(val); break;
		case 9:  val ^= (uint8_t)__builtin_popcount(val); break;
		case 10: val = rl8(val, val); break;
		case 11: val = rl8(val, 1); break;
		case 12: val ^= rl8(val, 2); break;
		case 13: val = rl8(val, 3); break;
		case 14: val ^= rl8(val, 4); break;
		case 15: val = rl8(val, 5); break;
		}
	}
	return val;
}

/* ---- AVX2 verify: test all 256 opcodes against scalar ---- */
#if PIKE_HAS_AVX2_HEADER
bool verify_avx2_wolf()
{
#ifdef NDEBUG
	return true;
#endif
	uint8_t in[288] = {0};
	uint8_t out_s[288], out_v[288];

	for (int i = 0; i < 288; i++) in[i] = (uint8_t)(i * 73 + 17);

	int bad = 0;
	for (int op = 0; op < 256; op++) {
		for (int p1 = 0; p1 < 256; p1 += 37) {
			for (int p2 = p1 + 1; p2 <= std::min(p1 + 32, 255); p2 += 7) {
				memcpy(out_s, in, 288);
				memcpy(out_v, in, 288);

				uint32_t opc = CodeLUT[op];
				for (int i = p1; i < p2; ++i)
					out_s[i] = wolfBranch(in[i], in[p2], opc);

				wolfPermute_avx2(in, out_v, (uint8_t)op, (uint8_t)p1, (uint8_t)p2);

				if (memcmp(out_s + p1, out_v + p1, p2 - p1)) {
					bad++;
					if (bad <= 3) {
						fprintf(stderr, "[verify] MISMATCH op=%d p1=%d p2=%d\n",
						        op, p1, p2);
					}
				}
			}
		}
	}

	if (!bad) {
		printf("[verify] AVX2 wolfPermute: PASS (256 opcodes)\n");
		return true;
	}
	fprintf(stderr, "[verify] AVX2 wolfPermute: FAIL (%d mismatches)\n", bad);
	return false;
}
#endif

/* ---- init_lut: build 1D table at startup (38 KB, fits L1) ---- */

void init_lut()
{
	if (g_lut_ready) return;

	memset(g_is_branched, 0, 256);
	memset(g_reg_idx, 0xFF, 256);
	uint8_t rc = 0;

	for (int op = 0; op < 256; op++) {
		bool br = false;
		for (auto b : branchedOps_global)
			if (b == op) { br = true; break; }
		if (br)
			g_is_branched[op] = 1;
		else
			g_reg_idx[op] = rc++;
	}

	/* 152 regular ops x 256 inputs = 38,912 bytes */
	lookup1D = (uint8_t*)std::malloc(rc * 256);
	if (!lookup1D) {
		fprintf(stderr, "[dluna] 1D LUT alloc failed\n");
		return;
	}

	for (int op = 0; op < 256; op++) {
		if (g_is_branched[op]) continue;
		uint8_t idx = g_reg_idx[op];
		uint32_t opc = CodeLUT[op];
		for (int v = 0; v < 256; v++)
			lookup1D[idx * 256 + v] = wolfBranch((uint8_t)v, 0, opc);
	}

	g_lut_ready = true;
}

/* ---- wolfCompute: the 278-iteration core loop ---- */

static void wolfCompute(workerData& w)
{
        w.templateIdx = 0;
        uint8_t chunkCount = 1;
        int firstChunk = 0;
        uint8_t lp1 = 0, lp2 = 255;
        w.tries[0] = 0;

        for (int it = 0; it < 278; ++it) {
                w.tries[0]++;

                /* (a) random_switcher from hash state */
                w.random_switcher = w.prev_lhash ^ w.lhash ^ w.tries[0];       

                w.op = (byte)w.random_switcher;
                byte p1 = (byte)(w.random_switcher >> 8);
                byte p2 = (byte)(w.random_switcher >> 16);

                /* (c) clamp range */
                if (p1 > p2) std::swap(p1, p2);
                if (p2 - p1 > 32) p2 = p1 + ((p2 - p1) & 0x1f);

                lp1 = std::min(lp1, p1);
                lp2 = std::max(lp2, p2);

                w.pos1 = p1;
                w.pos2 = p2;
                w.chunk = &w.sData[(w.tries[0] - 1) * 256];

                /* (d) Full chunk copy (incremental was causing parity issues) */
                if (w.tries[0] == 1) {
                        w.prev_chunk = w.chunk;
                } else {
                        w.prev_chunk = &w.sData[(w.tries[0] - 2) * 256];       
                        memcpy(w.chunk, w.prev_chunk, 256);
                }

                /* (e) prefetch next chunk */
                if (it + 1 < 278)
                        __builtin_prefetch(&w.sData[w.tries[0] * 256], 1, 2);  

                /* op 253 special case */
                if (w.op == 253) {
                        for (int i = p1; i < p2; i++) {
                                w.chunk[i] = rl8(w.chunk[i], 3);
                                w.chunk[i] ^= rl8(w.chunk[i], 2);
                                w.chunk[i] ^= w.prev_chunk[p2];
                                w.chunk[i] = rl8(w.chunk[i], 3);

                                w.prev_lhash = w.lhash + w.prev_lhash;
                                w.lhash = XXHash64::hash(w.chunk, p2, 0);
                        }
                        goto after_op;
                }

                /* zero-fill ops (decoded 2026-04-25 from corrected
                 * branch_compute.bin: ops 53, 55, 188, 249 have ~142-148
                 * byte handlers that are pure `mov BYTE [..],0x0` loops —
                 * NOT representable in the 4-micro-op CodeLUT model. */
                if (w.op == 53 || w.op == 55 || w.op == 188 || w.op == 249) {
                        /* zero-fill confirmed by RE of branch_compute.bin handlers
                         * @ 0x26298, 0x25fc8, 0x0de60, 0x023f2 (sizes 142-148).
                         * Matches existing AVX2/LUT dispatch output, kept as a
                         * stable explicit case. */
                        for (int i = p1; i < p2; i++) {
                                w.chunk[i] = 0;
                        }
                        goto after_op;
                }

                /* (f) RC4 re-key on op>=254 */
                if (w.op >= 254)
                        RC4_set_key(&w.key[0], 256, w.prev_chunk);

                /* (g) hybrid dispatch */
                if (g_lut_ready && !g_is_branched[w.op] && w.op < 253) {       
                        /* 1D LUT: one memory lookup per byte */
                        const uint8_t* lut = &lookup1D[g_reg_idx[w.op] * 256]; 
                        for (int i = p1; i < p2; ++i)
                                w.chunk[i] = lut[w.prev_chunk[i]];
                }
#if PIKE_HAS_AVX2_HEADER
                else if (g_has_avx2) {
                        wolfPermute_avx2(w.prev_chunk, w.chunk, w.op, p1, p2); 
                }
#endif
                else {
                        /* scalar fallback */
                        uint32_t opc = CodeLUT[w.op];
                        for (int i = p1; i < p2; ++i)
                                w.chunk[i] = wolfBranch(w.prev_chunk[i], w.prev_chunk[p2], opc);
                }

                /* (h) op 0 special: byte swap + reverse if odd range */       
                if (!w.op) {
                        if ((p2 - p1) % 2 == 1) {
                                uint8_t t1 = w.chunk[p1];
                                uint8_t t2 = w.chunk[p2];
                                w.chunk[p1] = reverse8(t2);
                                w.chunk[p2] = reverse8(t1);
                        }
                }

after_op:
                /* (i) A = chunk[p1] - chunk[p2] */
                w.A = (w.chunk[p1] - w.chunk[p2]);

                /* (j) conditional hashing */
                {
                        const int hash_sel = (w.A < 0x30) + (w.A < 0x20) + (w.A < 0x10);
                        switch (hash_sel) {
                        case 3: /* A < 0x10 */
                                w.prev_lhash = w.lhash + w.prev_lhash;
                                w.lhash = XXHash64::hash(w.chunk, p2, 0);
                                [[fallthrough]];
                        case 2: /* A < 0x20 */
                                w.prev_lhash = w.lhash + w.prev_lhash;
                                w.lhash = hash_64_fnv1a(w.chunk, p2);
                                [[fallthrough]];
                        case 1: /* A < 0x30 */
                                w.prev_lhash = w.lhash + w.prev_lhash;
                                alignas(16) const unsigned long long key2[2] = {
                                        (unsigned long long)w.tries[0], (unsigned long long)w.prev_lhash
                                };
                                w.lhash = highwayhash::SipHash(key2, (char*)w.chunk, (unsigned long long)p2);
                                break;
                        }
                }

                /* (k) RC4 encrypt + record templateMarker for SPSA */
                if (w.A <= 0x40) {
                        RC4(&w.key[0], 256, w.chunk, w.chunk);
                        w.astroTemplate[w.templateIdx] = templateMarker{
                                (uint8_t)(chunkCount > 1 ? lp1 : 0),
                                (uint8_t)(chunkCount > 1 ? lp2 : 255),
                                0, 0,
                                (uint16_t)((firstChunk << 7) | chunkCount)
                        };
                        w.templateIdx += (w.tries[0] > 1);
                        firstChunk = w.tries[0] - 1;
                        lp1 = 255; lp2 = 0; chunkCount = 1;
                } else {
                        chunkCount++;
                }

                /* (l) XOR mix */
                w.chunk[255] = w.chunk[255] ^ w.chunk[p1] ^ w.chunk[p2];

                /* (m) break condition */
                if (w.tries[0] > 276 ||
                    (w.chunk[255] >= 0xf0 && w.tries[0] > 260))
                        break;
        }

        /* flush final template marker */
        if (chunkCount > 0) {
                w.astroTemplate[w.templateIdx++] = templateMarker{
                        (uint8_t)(chunkCount > 1 ? lp1 : 0),
                        (uint8_t)(chunkCount > 1 ? lp2 : 255),
                        0, 0,
                        (uint16_t)((firstChunk << 7) | chunkCount)
                };
        }

        /* data_len from tries and final chunk bytes */
        w.data_len = (uint32_t)(
                (w.tries[0] - 4) * 256 +
                (((uint64_t)w.chunk[253] << 8 | (uint64_t)w.chunk[254]) & 0x3ff));
        /* v1.14 passes the non-zero logical prefix into encode; zero tail bytes
         * after the computed length are padding, not part of the SA input. */
        while (w.data_len > 0 && w.sData[w.data_len - 1] == 0) {
                --w.data_len;
        }

        /* Tripwire for the SA tail-slack contract: every SA output buffer must
         * hold data_len + 7 elements. Enable with DLUNA_TRACK_DATALEN=1. */
        static const bool track_len = [] {
                const char* e = getenv("DLUNA_TRACK_DATALEN");
                return e && e[0] && strcmp(e, "0") != 0;
        }();
        if (track_len) {
                static std::atomic<uint32_t> max_len{0};
                uint32_t prev = max_len.load(std::memory_order_relaxed);
                while (w.data_len > prev &&
                       !max_len.compare_exchange_weak(prev, w.data_len,
                                                      std::memory_order_relaxed)) {
                }
                if (w.data_len > prev)
                        fprintf(stderr, "[DATALEN] new max=%u\n", w.data_len);
        }
}

/* 2026-05-01 stage-3 replay wrapper accessor.
 * Reuses the existing wolfCompute loop. The replay input schema for this
 * first harness round is [u32 len][buffer]; state that v1.14 carries outside
 * that buffer will surface as divergence in the parity report. */
uint32_t dluna_internal_branch_dispatch(const uint8_t* in_buf, uint32_t in_buf_len,
                                        uint8_t* out_buf, uint32_t out_buf_cap)
{
	if (!in_buf || !out_buf || in_buf_len == 0 || out_buf_cap < ASTRO_SCRATCH_SIZE) {
		return 0;
	}
	auto w = std::make_unique<workerData>();
	memset(w.get(), 0, sizeof(workerData));
	uint32_t copy_len = in_buf_len;
	if (copy_len > ASTRO_SCRATCH_SIZE) copy_len = ASTRO_SCRATCH_SIZE;
	memcpy(w->sData, in_buf, copy_len);
	if (in_buf_len == 256) {
		RC4_set_key(&w->key[0], 256, w->sData);
		RC4(&w->key[0], 256, w->sData, w->sData);
	} else {
		RC4_set_key(&w->key[0], 256, w->sData);
	}
	w->lhash = hash_64_fnv1a_256(w->sData);
	w->prev_lhash = w->lhash;
	wolfCompute(*w);
	if (w->data_len == 0 || w->data_len > out_buf_cap) return 0;
	memcpy(out_buf, w->sData, w->data_len);
	return w->data_len;
}
/* Phase-A profiling 2026-04-29 — diagnosed libsais as 90% of hash time.
 * Per-stage clock accumulators. Each thread dumps its own breakdown every
 * PROF_FLUSH_HASHES hashes when DLUNA_PROFILE=1 is set in the env.
 * Off-by-default so production hot path is unaffected. */
static inline uint64_t prof_rdtsc() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
namespace {
    thread_local uint64_t prof_ticks[7] = {0};   /* 0=prelude,1=salsa+rc4+fnv,2=wolfCompute,3=SPSA-call,4=stage5,5=sha-over-sa,6=total */
    thread_local uint64_t prof_hashes = 0;
    thread_local int      prof_tid = -1;
    thread_local bool     prof_inited = false;
    static std::atomic<int> prof_next_tid{0};
    static bool prof_enabled = []{ const char* e = std::getenv("DLUNA_PROFILE"); return e && e[0] == '1'; }();
    static constexpr uint64_t PROF_FLUSH_HASHES = 1024;
    inline void prof_init() {
        if (!prof_inited) { prof_tid = prof_next_tid++; prof_inited = true; }
    }
    inline void prof_flush() {
        if (!prof_enabled || prof_hashes < PROF_FLUSH_HASHES) return;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        static constexpr const char* unit_per_hash = "cyc/hash";
#else
        static constexpr const char* unit_per_hash = "ns/hash";
#endif
        uint64_t total = prof_ticks[6] ? prof_ticks[6] : 1;
        printf("[PROF tid=%d hashes=%llu %s=%llu  prelude=%5.1f%%  salsa+rc4=%5.1f%%  wolfCompute=%5.1f%%  SPSA=%5.1f%%  stage5=%5.1f%%  shaOverSA=%5.1f%%]\n",
            prof_tid, (unsigned long long)prof_hashes, unit_per_hash,
            (unsigned long long)(total / prof_hashes),
            100.0 * prof_ticks[0] / total, 100.0 * prof_ticks[1] / total,
            100.0 * prof_ticks[2] / total, 100.0 * prof_ticks[3] / total,
            100.0 * prof_ticks[4] / total, 100.0 * prof_ticks[5] / total);
        for (int i = 0; i < 7; i++) prof_ticks[i] = 0;
        prof_hashes = 0;
    }

    struct LiveStage5Scratch {
        std::vector<uint8_t> flags;
        std::vector<uint8_t> desc;
        std::vector<uint8_t> arena;
        std::vector<uint8_t> descriptor_sa;
        uint64_t prof_flags = 0;
        uint64_t prof_encode = 0;
        uint64_t prof_sa = 0;
        uint64_t prof_calls = 0;
    };

    inline bool env_flag_enabled(const char* name) {
        const char* e = std::getenv(name);
        return e && e[0] == '1';
    }

    inline bool env_flag_disabled(const char* name) {
        const char* e = std::getenv(name);
        return e && e[0] == '0';
    }

    inline LiveStage5Scratch& live_stage5_scratch() {
        // Leak intentionally: this tree already avoids MinGW emutls vector
        // destructors in bench-only Stage 5 scratch paths.
        static thread_local LiveStage5Scratch* scratch = nullptr;
        if (!scratch) scratch = new LiveStage5Scratch;
        return *scratch;
    }

    bool build_v114_stage5_flags(const workerData& w, uint32_t logical_len,
                                 std::vector<uint8_t>* flags) {
        if (!flags || logical_len == 0 || w.templateIdx < 0) return false;

        const uint32_t full_groups = logical_len >> 8;
        flags->assign(static_cast<size_t>(full_groups) + 1u, 0);
        (*flags)[0] = 1;

        const int limit = std::min(w.templateIdx, 277);
        for (int i = 0; i < limit; ++i) {
            const uint32_t pos_data = w.astroTemplate[i].posData;
            const uint32_t start_group = pos_data >> 7;
            const uint32_t group_count = pos_data & 0x7fu;
            const uint32_t boundary = start_group + group_count;
            if (group_count != 0 && boundary > 0 && boundary < flags->size()) {
                (*flags)[boundary] = 1;
            }
        }

        return true;
    }

    bool build_v114_descriptor_sa(workerData& w, LiveStage5Scratch& scratch,
                                  uint8_t* sa_out, size_t sa_cap,
                                  size_t* sa_len) {
        if (sa_len) *sa_len = 0;
        if (!sa_out || !sa_len || w.data_len == 0) return false;

        static const bool desc_prof =
            []{ return env_flag_enabled("DLUNA_PROFILE_STAGE5_DESCRIPTOR"); }();
        const uint64_t p0 = desc_prof ? prof_rdtsc() : 0;
        if (!build_v114_stage5_flags(w, w.data_len, &scratch.flags)) {
            return false;
        }
        const uint64_t p1 = desc_prof ? prof_rdtsc() : 0;

        const size_t needed_sa = static_cast<size_t>(w.data_len) * 4u;
        if (sa_cap < needed_sa) return false;
        const uint32_t data_len_with_tail = w.data_len + 3u;
        const uint64_t p2 = desc_prof ? prof_rdtsc() : 0;

        const bool ok = deroluna::stages::v114::stage_v114_sa_build_compact_fused_raw(
                w.sData, w.data_len, data_len_with_tail,
                scratch.flags.data(), static_cast<uint32_t>(scratch.flags.size()),
                sa_out, sa_cap, sa_len);
        if (desc_prof) {
            const uint64_t p3 = prof_rdtsc();
            scratch.prof_flags += p1 - p0;
            scratch.prof_encode += p2 - p1;
            scratch.prof_sa += p3 - p2;
            ++scratch.prof_calls;
            if (scratch.prof_calls >= 1024) {
                const uint64_t calls = scratch.prof_calls ? scratch.prof_calls : 1;
                std::fprintf(stderr,
                    "[DESC_PROF calls=%llu flags=%llu encode=%llu descriptor_sa=%llu]\n",
                    (unsigned long long)scratch.prof_calls,
                    (unsigned long long)(scratch.prof_flags / calls),
                    (unsigned long long)(scratch.prof_encode / calls),
                    (unsigned long long)(scratch.prof_sa / calls));
                scratch.prof_flags = 0;
                scratch.prof_encode = 0;
                scratch.prof_sa = 0;
                scratch.prof_calls = 0;
            }
        }
        return ok;
    }

    bool build_v114_descriptor_hash(workerData& w, LiveStage5Scratch& scratch,
                                    uint8_t hash_out[32]) {
        if (!hash_out || w.data_len == 0) return false;

        static const bool desc_prof =
            []{ return env_flag_enabled("DLUNA_PROFILE_STAGE5_DESCRIPTOR"); }();
        const uint64_t p0 = desc_prof ? prof_rdtsc() : 0;
        if (!build_v114_stage5_flags(w, w.data_len, &scratch.flags)) {
            return false;
        }
        const uint64_t p1 = desc_prof ? prof_rdtsc() : 0;

        const uint32_t data_len_with_tail = w.data_len + 3u;
        const uint64_t p2 = desc_prof ? prof_rdtsc() : 0;
        const bool ok = deroluna::stages::v114::stage_v114_hash_compact_fused_raw(
                w.sData, w.data_len, data_len_with_tail,
                scratch.flags.data(), static_cast<uint32_t>(scratch.flags.size()),
                hash_out);
        if (desc_prof) {
            const uint64_t p3 = prof_rdtsc();
            scratch.prof_flags += p1 - p0;
            scratch.prof_encode += p2 - p1;
            scratch.prof_sa += p3 - p2;
            ++scratch.prof_calls;
            if (scratch.prof_calls >= 1024) {
                const uint64_t calls = scratch.prof_calls ? scratch.prof_calls : 1;
                std::fprintf(stderr,
                    "[DESC_PROF calls=%llu flags=%llu encode=%llu descriptor_sa=%llu]\n",
                    (unsigned long long)scratch.prof_calls,
                    (unsigned long long)(scratch.prof_flags / calls),
                    (unsigned long long)(scratch.prof_encode / calls),
                    (unsigned long long)(scratch.prof_sa / calls));
                scratch.prof_flags = 0;
                scratch.prof_encode = 0;
                scratch.prof_sa = 0;
                scratch.prof_calls = 0;
            }
        }
        return ok;
    }
}

extern "C" void sa_construct_2byte(const uint8_t*, int32_t*, int32_t);

#if defined(USE_ASTRO_SPSA) && (defined(__x86_64__) || defined(_M_X64))
/* Forward decl for Tritonn entry point (x86-only verify path) */
extern "C" uint64_t SPSA_tritonn_entry(const uint8_t* in, int len, ::workerData& w);
extern "C" void dbg_sha_capture_set(int on);
extern "C" const uint8_t* dbg_sha_capture_data(size_t* len_out);
extern "C" void dbg_emit_reset();
extern "C" void dbg_emit_get(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);
#endif

/* ---- dluna_hash: the complete AstroBWT v3 hash function ---- */

struct DlunaHashProf {
	uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
};

static void dluna_hash_prof_tail(const DlunaHashProf& p)
{
	if (!prof_enabled) return;
	prof_init();
	uint64_t tend = prof_rdtsc();
	prof_ticks[0] += (p.t1 - p.t0);
	prof_ticks[1] += (p.t2 - p.t1);
	prof_ticks[2] += (p.t3 - p.t2);
	prof_ticks[3] += (p.t4 - p.t3);
	prof_ticks[4] += (p.t5 - p.t4);
	prof_ticks[5] += (tend - p.t5);
	prof_ticks[6] += (tend - p.t0);
	prof_hashes++;
	prof_flush();
}

enum { DLUNA_HASH_SA_READY = 0, DLUNA_HASH_DONE = 1 };

/* Stages 1-6: prologue through the suffix array. Returns SA_READY when the
 * caller owes the final SHA-256 over w.sa[0 .. data_len*4); DONE when output
 * already holds the hash (SPSA padding short-circuit, or the env-gated
 * streamed descriptor hash — reachable only when allow_stream_hash). */
static int dluna_hash_prepare(byte* input, int inputLen, byte* output,
                              workerData& w, bool allow_stream_hash,
                              DlunaHashProf& prof)
{
	uint8_t scratch[384] = {0};
	uint64_t t0 = prof_enabled ? prof_rdtsc() : 0;

	/* SHA256(input) -> scratch[320..352] */
	hashSHA256(input, &scratch[320], inputLen);
	uint64_t t1 = prof_enabled ? prof_rdtsc() : 0;

	/* Salsa20(key=scratch[320], iv=scratch[256]) -> scratch[0..256] */
	dluna_internal_salsa20_expand(&scratch[320], scratch);

	/* RC4(key=scratch, encrypt=scratch) -> scratch[0..256] */
	RC4_set_key(&w.key[0], 256, scratch);
	RC4(&w.key[0], 256, scratch, scratch);

	/* FNV-1a hash of scratch -> lhash */
	w.lhash = hash_64_fnv1a_256(scratch);
	w.prev_lhash = w.lhash;
	w.tries[0] = 0;
	memcpy(w.sData, scratch, 256);
	uint64_t t2 = prof_enabled ? prof_rdtsc() : 0;

	/* wolfCompute: 278 iterations of branch-compute */
	wolfCompute(w);
	uint64_t t3 = prof_enabled ? prof_rdtsc() : 0;

	/* zero pad after data */

	memset(w.sData + w.data_len, 0, 16);

#if defined(USE_ASTRO_SPSA)
	memset(w.padding, 0, 32);
	SPSA(w.sData, w.data_len, w);
	uint64_t t4 = prof_enabled ? prof_rdtsc() : 0;
	bool padding_nonzero = false;
	for (int i = 0; i < 32; i++) if (w.padding[i] != 0) { padding_nonzero = true; break; }
	if (padding_nonzero) {
		memcpy(output, w.padding, 32);
		prof.t0 = t0; prof.t1 = t1; prof.t2 = t2; prof.t3 = t3;
		prof.t4 = t4; prof.t5 = t4;
		return DLUNA_HASH_DONE;
	}
#else
	uint64_t t4 = prof_enabled ? prof_rdtsc() : 0;
#endif

        /* SA construction. Default: v1.14 descriptor/arena tail.
         * Fallback: DLUNA_FORCE_LIBSAIS=1 or DLUNA_USE_STAGE5_DESCRIPTOR=0.
         * Byte-check oracle mode: DLUNA_VERIFY_STAGE5_DESCRIPTOR=1.
         * The older 2-byte radix path remains as a negative-control stub. */
        static const bool sa_verify_descriptor =
            []{ return env_flag_enabled("DLUNA_VERIFY_STAGE5_DESCRIPTOR"); }();
        static const bool sa_use_2byte =
            []{ return env_flag_enabled("DLUNA_USE_2BYTE_SA"); }();
        static const bool sa_use_descriptor =
            []{
                if (env_flag_enabled("DLUNA_USE_2BYTE_SA")) return false;
                if (env_flag_enabled("DLUNA_FORCE_LIBSAIS")) return false;
                if (env_flag_disabled("DLUNA_USE_STAGE5_DESCRIPTOR")) return false;
                return true;
            }();
        static const bool sa_materialize_for_tritonn =
            []{
                return env_flag_enabled("DLUNA_VERIFY_TRITONN") ||
                       env_flag_enabled("DLUNA_VERIFY_TRITONN_STATS");
            }();
        static const bool sa_stream_descriptor_hash =
            []{ return env_flag_enabled("DLUNA_STREAM_STAGE5_HASH"); }();
        static thread_local void* libsais_ctx_tl = nullptr;
        static std::atomic<uint64_t> desc_build_failures{0};
        static std::atomic<uint64_t> desc_mismatches{0};
        static std::atomic<uint64_t> desc_matches{0};

        auto run_libsais_sa = [&]() {
            if (__builtin_expect(libsais_ctx_tl == nullptr, 0)) {
                libsais_ctx_tl = libsais_create_ctx();
            }
            libsais_ctx(libsais_ctx_tl, w.sData, w.sa, w.data_len, 0, nullptr);
        };
        auto run_default_sa = [&]() {
            if (sa_use_2byte) {
                sa_construct_2byte(w.sData, w.sa, w.data_len);
            } else {
                run_libsais_sa();
            }
        };

        LiveStage5Scratch* desc_scratch = nullptr;
        size_t desc_sa_len = 0;
        bool desc_ready = false;
        bool descriptor_hash_ready = false;
        const bool can_stream_descriptor_hash =
            allow_stream_hash && sa_stream_descriptor_hash && sa_use_descriptor &&
            !sa_verify_descriptor && !sa_materialize_for_tritonn;
        if (can_stream_descriptor_hash) {
            desc_scratch = &live_stage5_scratch();
            descriptor_hash_ready = build_v114_descriptor_hash(
                w, *desc_scratch, output);
            if (!descriptor_hash_ready) {
                const uint64_t n = desc_build_failures.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    std::fprintf(stderr,
                        "[DLUNA_STAGE5_DESCRIPTOR] hash build failed data_len=%u flags=%zu; falling back to libsais\n",
                        w.data_len,
                        desc_scratch ? desc_scratch->flags.size() : 0u);
                }
            }
        } else if (sa_use_descriptor || sa_verify_descriptor) {
            desc_scratch = &live_stage5_scratch();
            /* Pass the real buffer capacity, not the logical SA size: the
             * materialize step needs >= 28 bytes of tail slack beyond
             * data_len*4 before it may use 32-byte block stores. */
            size_t sa_cap = sizeof(w.sa);
            uint8_t* desc_out = reinterpret_cast<uint8_t*>(w.sa);
            if (sa_verify_descriptor) {
                const size_t verify_cap =
                    static_cast<size_t>(w.data_len) * 4u + 32u;
                if (desc_scratch->descriptor_sa.size() < verify_cap) {
                    desc_scratch->descriptor_sa.resize(verify_cap);
                }
                desc_out = desc_scratch->descriptor_sa.data();
                sa_cap = desc_scratch->descriptor_sa.size();
            }
            desc_ready = build_v114_descriptor_sa(w, *desc_scratch,
                                                  desc_out, sa_cap,
                                                  &desc_sa_len) &&
                         desc_sa_len == static_cast<size_t>(w.data_len) * 4u;
            if (!desc_ready) {
                const uint64_t n = desc_build_failures.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    std::fprintf(stderr,
                        "[DLUNA_STAGE5_DESCRIPTOR] build failed data_len=%u flags=%zu sa_len=%zu; falling back to libsais\n",
                        w.data_len,
                        desc_scratch ? desc_scratch->flags.size() : 0u,
                        desc_sa_len);
                }
            }
        }

        if (sa_verify_descriptor) {
            run_libsais_sa();
            if (desc_ready) {
                const size_t sa_bytes = static_cast<size_t>(w.data_len) * 4u;
                const uint8_t* lib_bytes = reinterpret_cast<const uint8_t*>(w.sa);
                const int cmp = std::memcmp(desc_scratch->descriptor_sa.data(),
                                            lib_bytes, sa_bytes);
                if (cmp == 0) {
                    const uint64_t n = desc_matches.fetch_add(1, std::memory_order_relaxed);
                    if (n == 0) {
                        std::fprintf(stderr,
                            "[DLUNA_STAGE5_DESCRIPTOR] verify byte-match data_len=%u\n",
                            w.data_len);
                    }
                    if (sa_use_descriptor) {
                        std::memcpy(w.sa, desc_scratch->descriptor_sa.data(), sa_bytes);
                    }
                } else {
                    size_t first = 0;
                    while (first < sa_bytes &&
                           desc_scratch->descriptor_sa[first] == lib_bytes[first]) {
                        ++first;
                    }
                    const uint64_t n = desc_mismatches.fetch_add(1, std::memory_order_relaxed);
                    if (n < 8) {
                        std::fprintf(stderr,
                            "[DLUNA_STAGE5_DESCRIPTOR] verify mismatch data_len=%u byte=%zu desc=%02x libsais=%02x; using libsais\n",
                            w.data_len, first,
                            first < sa_bytes ? desc_scratch->descriptor_sa[first] : 0,
                            first < sa_bytes ? lib_bytes[first] : 0);
                    }
                }
            }
        } else if (descriptor_hash_ready) {
            /* Non-verify descriptor mode streamed the fused SA bytes into output. */
        } else if (sa_use_descriptor && desc_ready) {
            /* Non-verify descriptor mode writes directly into w.sa. */
        } else {
            run_default_sa();
        }
        uint64_t t5 = prof_enabled ? prof_rdtsc() : 0;
#if defined(USE_ASTRO_SPSA) && (defined(__x86_64__) || defined(_M_X64))
	/* ---- VALIDATION HARNESS: libsais vs Tritonn-port byte-for-byte ---- */
	static const bool sa_verify_tritonn = []{ const char* e = std::getenv("DLUNA_VERIFY_TRITONN"); return e && e[0] == '1'; }();
	static const bool sa_verify_stats   = []{ const char* e = std::getenv("DLUNA_VERIFY_TRITONN_STATS"); return e && e[0] == '1'; }();
	static thread_local struct {
		uint64_t total_positions = 0, total_mismatches = 0, hash_count = 0;
		uint64_t sentinel_leak = 0, off_by_stride = 0, order_swap = 0, missing = 0;
		uint64_t first_diverge_sum = 0, first_diverge_count = 0;
	} tritonn_stats;

	if (sa_verify_tritonn || sa_verify_stats) {
		int32_t libsais_sa[71000];
		memcpy(libsais_sa, w.sa, w.data_len * sizeof(int32_t));
		/* Compute the libsais-side digest for hash-level comparison. */
		uint8_t libsais_digest[32];
		hashSHA256((byte*)w.sa, libsais_digest, w.data_len * 4);
		/* Clear w.padding so we can detect whether Tritonn writes a digest. */
		memset(w.padding, 0, 32);
		/* Capture decompress's SHA256 input byte stream for byte-level diff. */
		dbg_sha_capture_set(1);
		dbg_emit_reset();
		uint64_t tritonn_ret = SPSA_tritonn_entry(w.sData, w.data_len, w);
		dbg_sha_capture_set(0);
		(void)tritonn_ret;
		if (sa_verify_tritonn) {
			uint64_t pto, ps, dc_lit, dc_comp, dc_pos;
			dbg_emit_get(&pto, &ps, &dc_lit, &dc_comp, &dc_pos);
			fprintf(stderr, "[COUNTS] data_len=%u  pto=%llu ps=%llu  dc_lit=%llu dc_comp=%llu dc_pos=%llu  pto+ps=%llu  dc_total=%llu\n",
				w.data_len, (unsigned long long)pto, (unsigned long long)ps,
				(unsigned long long)dc_lit, (unsigned long long)dc_comp, (unsigned long long)dc_pos,
				(unsigned long long)(pto+ps), (unsigned long long)(dc_lit+dc_pos));
		}
		/* BYTE-STREAM DIFF: libsais's SA bytes vs Tritonn's decompress output bytes.
		 * Both should produce identical bytes if Tritonn-port is correct. */
		if (sa_verify_tritonn) {
			size_t trit_len = 0;
			const uint8_t* trit_stream = dbg_sha_capture_data(&trit_len);
			size_t lib_len = (size_t)w.data_len * 4u;
			const uint8_t* lib_stream = (const uint8_t*)libsais_sa;
			if (trit_len != lib_len) {
				fprintf(stderr, "[STREAM] LEN-MISMATCH libsais=%zu tritonn=%zu (delta=%zd)\n",
					lib_len, trit_len, (ptrdiff_t)trit_len - (ptrdiff_t)lib_len);
			} else {
				int first_diff = -1;
				size_t cmp_len = lib_len < trit_len ? lib_len : trit_len;
				for (size_t i = 0; i < cmp_len; i++) {
					if (lib_stream[i] != trit_stream[i]) { first_diff = (int)i; break; }
				}
				if (first_diff < 0) {
					fprintf(stderr, "[STREAM] BYTE-MATCH (%zu bytes)\n", cmp_len);
				} else {
					fprintf(stderr, "[STREAM] DIVERGE at byte %d (sa[%d]+%d): libsais=%02x tritonn=%02x\n",
						first_diff, first_diff/4, first_diff%4,
						lib_stream[first_diff], trit_stream[first_diff]);
					/* Print 16-byte context on each side */
					int ctx_lo = first_diff > 16 ? first_diff - 16 : 0;
					int ctx_hi = (size_t)(first_diff + 16) < cmp_len ? first_diff + 16 : (int)cmp_len - 1;
					fprintf(stderr, "  libsais[%d..%d]:", ctx_lo, ctx_hi);
					for (int i = ctx_lo; i <= ctx_hi; i++) fprintf(stderr, " %02x", lib_stream[i]);
					fprintf(stderr, "\n  tritonn[%d..%d]:", ctx_lo, ctx_hi);
					for (int i = ctx_lo; i <= ctx_hi; i++) fprintf(stderr, " %02x", trit_stream[i]);
					fprintf(stderr, "\n");
				}
			}
		}
		/* HASH-LEVEL CHECK: Tritonn's decompress writes its 32-byte SHA256
		 * to w.padding. If it matches libsais_digest, the port is end-to-end
		 * correct (any per-position differences in sa_prelim are bucket-order
		 * artifacts, not bugs). */
		bool tritonn_wrote_digest = false;
		for (int i = 0; i < 32; i++) if (w.padding[i] != 0) { tritonn_wrote_digest = true; break; }
		bool digest_match = tritonn_wrote_digest && memcmp(w.padding, libsais_digest, 32) == 0;
		if (sa_verify_tritonn) {
			if (digest_match) {
				fprintf(stderr, "[VERIFY_TRITONN] DIGEST MATCH end-to-end\n");
			} else if (tritonn_wrote_digest) {
				fprintf(stderr, "[VERIFY_TRITONN] DIGEST DIFFER libsais=%02x%02x%02x%02x.. tritonn=%02x%02x%02x%02x..\n",
					libsais_digest[0], libsais_digest[1], libsais_digest[2], libsais_digest[3],
					w.padding[0], w.padding[1], w.padding[2], w.padding[3]);
			} else {
				fprintf(stderr, "[VERIFY_TRITONN] tritonn produced no digest (decompress incomplete)\n");
			}
		}

		bool sa_diverge = false;
		int diverge_idx = -1;
		int32_t libval = 0;
		uint32_t tritval = 0;
		for (int i = 0; i < (int)w.data_len && i < 71000; i++) {
			if (libsais_sa[i] != (int32_t)w.sa_prelim[i]) {
				sa_diverge = true; diverge_idx = i;
				libval = libsais_sa[i]; tritval = w.sa_prelim[i];
				break;
			}
		}

		if (sa_verify_tritonn) {
			if (sa_diverge) {
				fprintf(stderr, "[VERIFY_TRITONN] DIVERGE at SA[%d]: libsais=%d tritonn=%u\n",
					diverge_idx, libval, tritval);
			} else {
				fprintf(stderr, "[VERIFY_TRITONN] PASS: SA[0..%u) match byte-for-byte\n", w.data_len);
			}
		}

		if (sa_verify_stats) {
			tritonn_stats.total_positions += w.data_len;
			tritonn_stats.hash_count++;
			if (sa_diverge) {
				tritonn_stats.total_mismatches++;
				tritonn_stats.first_diverge_sum += diverge_idx;
				tritonn_stats.first_diverge_count++;
				if (tritval == 0xFFFFFFFFU) {
					tritonn_stats.sentinel_leak++;
				} else if (libval >= 0 && tritval < (uint32_t)w.data_len) {
					int32_t diff = libval - (int32_t)tritval;
					if (diff != 0 && diff % 256 == 0) {
						tritonn_stats.off_by_stride++;
					} else {
						bool lib_in_first = false, trit_in_first = false;
						int scan_lim = diverge_idx < 256 ? diverge_idx : 256;
						for (int j = 0; j < scan_lim; j++) {
							if (libsais_sa[j] == (int32_t)tritval) trit_in_first = true;
							if ((int32_t)w.sa_prelim[j] == libval) lib_in_first = true;
						}
						if (lib_in_first && trit_in_first) tritonn_stats.order_swap++;
						else tritonn_stats.missing++;
					}
				} else tritonn_stats.missing++;
			}
			if ((tritonn_stats.hash_count % 256) == 0) {
				double match_rate = tritonn_stats.total_positions > 0
					? 100.0 * (1.0 - (double)tritonn_stats.total_mismatches / tritonn_stats.total_positions)
					: 100.0;
				double avg_first = tritonn_stats.first_diverge_count > 0
					? (double)tritonn_stats.first_diverge_sum / tritonn_stats.first_diverge_count : 0.0;
				fprintf(stderr,
					"[STATS] @h=%llu pos=%llu mism=%llu (%.2f%% match)  "
					"sentinel=%llu offstr=%llu swap=%llu miss=%llu  avg-first=%.0f (n=%llu)\n",
					(unsigned long long)tritonn_stats.hash_count,
					(unsigned long long)tritonn_stats.total_positions,
					(unsigned long long)tritonn_stats.total_mismatches,
					match_rate,
					(unsigned long long)tritonn_stats.sentinel_leak,
					(unsigned long long)tritonn_stats.off_by_stride,
					(unsigned long long)tritonn_stats.order_swap,
					(unsigned long long)tritonn_stats.missing,
					avg_first, (unsigned long long)tritonn_stats.first_diverge_count);
			}
		}
	}
	/* ---- END VALIDATION HARNESS ---- */
#endif // USE_ASTRO_SPSA

        prof.t0 = t0; prof.t1 = t1; prof.t2 = t2; prof.t3 = t3;
        prof.t4 = t4; prof.t5 = t5;
        return descriptor_hash_ready ? DLUNA_HASH_DONE : DLUNA_HASH_SA_READY;
}

void dluna_hash(byte* input, int inputLen, byte* output, workerData& w)
{
	DlunaHashProf prof;
	if (dluna_hash_prepare(input, inputLen, output, w, true, prof) ==
	    DLUNA_HASH_SA_READY) {
		hashSHA256((byte*)w.sa, output, w.data_len * 4);
	}
	dluna_hash_prof_tail(prof);
}

bool dluna_hash_x2_available(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	return dluna_sha256_x2_available();
#else
	return false;
#endif
}

void dluna_hash_x2(byte* input_a, byte* input_b, int inputLen,
                   byte* output_a, byte* output_b,
                   workerData& wa, workerData& wb)
{
	/* The streamed descriptor hash consumes the SA during the build, which
	 * leaves nothing for the batched SHA — the two are mutually exclusive,
	 * so the x2 path always materializes. */
	static const bool stream_requested =
	    []{ return env_flag_enabled("DLUNA_STREAM_STAGE5_HASH"); }();
	static std::atomic<bool> stream_warned{false};
	if (stream_requested && !stream_warned.exchange(true)) {
		fprintf(stderr,
		        "DLUNA_STREAM_STAGE5_HASH is ignored on the 2-way SHA path "
		        "(disable with DLUNA_NO_SHA_X2=1 to stream)\n");
	}

	DlunaHashProf prof_a, prof_b;
	int ra = dluna_hash_prepare(input_a, inputLen, output_a, wa, false, prof_a);
	int rb = dluna_hash_prepare(input_b, inputLen, output_b, wb, false, prof_b);
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	if (ra == DLUNA_HASH_SA_READY && rb == DLUNA_HASH_SA_READY &&
	    dluna_sha256_x2_available()) {
		dluna_sha256_x2(reinterpret_cast<const uint8_t*>(wa.sa),
		                static_cast<size_t>(wa.data_len) * 4u, output_a,
		                reinterpret_cast<const uint8_t*>(wb.sa),
		                static_cast<size_t>(wb.data_len) * 4u, output_b);
		ra = DLUNA_HASH_DONE;
		rb = DLUNA_HASH_DONE;
	}
#endif
	if (ra == DLUNA_HASH_SA_READY)
		hashSHA256((byte*)wa.sa, output_a, wa.data_len * 4);
	if (rb == DLUNA_HASH_SA_READY)
		hashSHA256((byte*)wb.sa, output_b, wb.data_len * 4);
	dluna_hash_prof_tail(prof_a);
	dluna_hash_prof_tail(prof_b);
}
