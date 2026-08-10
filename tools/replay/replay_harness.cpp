// replay_harness.cpp - Main entry for dirtybird-c-miner-replay.

#include "cap_format.h"
#include "report.h"
#include "../../include/dluna_stages.h"
#include "../../include/dluna_v114.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace deroluna::replay;

struct StageEntry {
    StageId id;
    deroluna::stages::StageFn fn;
    bool is_stub;
    size_t out_buf_cap;
};

static const StageEntry kStages[] = {
    {StageId::Salsa20Init,    deroluna::stages::stage_salsa20_init,        false,    256},
    {StageId::Rc4Ksa,         deroluna::stages::stage_rc4_ksa,             false,    256},
    {StageId::BranchDispatch, deroluna::stages::stage_branch_dispatch,     false, 200000},
    {StageId::V114Encode,     deroluna::stages::v114::stage_v114_encode,   false, 600000},
    {StageId::V114SaBuild,    deroluna::stages::v114::stage_v114_sa_build, false, 400000},
    {StageId::Sha256OfSa,     deroluna::stages::stage_sha256_of_sa,        false,     32},
};

struct Args {
    std::string captures;
    std::string report_out;
    uint32_t stage_mask = 0x3F;
    int max_diffs_per_stage = 16;
};

static bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing arg for %s\n", s.c_str());
                return nullptr;
            }
            return argv[++i];
        };

        if (s == "--captures") {
            const char* v = next(); if (!v) return false;
            args->captures = v;
        } else if (s == "--report-out") {
            const char* v = next(); if (!v) return false;
            args->report_out = v;
        } else if (s == "--max-diffs") {
            const char* v = next(); if (!v) return false;
            args->max_diffs_per_stage = std::atoi(v);
        } else if (s == "--stages") {
            const char* v = next(); if (!v) return false;
            args->stage_mask = 0;
            std::string vs = v;
            size_t pos = 0;
            while (pos <= vs.size()) {
                size_t comma = vs.find(',', pos);
                std::string token = vs.substr(pos, comma == std::string::npos
                                                       ? std::string::npos
                                                       : comma - pos);
                size_t dash = token.find('-');
                if (dash == std::string::npos) {
                    int n = std::atoi(token.c_str());
                    if (n >= 1 && n <= 6) args->stage_mask |= (1u << (n - 1));
                } else {
                    int lo = std::atoi(token.substr(0, dash).c_str());
                    int hi = std::atoi(token.substr(dash + 1).c_str());
                    if (lo > hi) std::swap(lo, hi);
                    for (int n = lo; n <= hi; ++n) {
                        if (n >= 1 && n <= 6) args->stage_mask |= (1u << (n - 1));
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (s == "-h" || s == "--help") {
            std::printf("Usage: dirtybird-c-miner-replay --captures DIR --report-out DIR "
                        "[--stages 1-6] [--max-diffs 16]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            return false;
        }
    }

    if (args->captures.empty() || args->report_out.empty()) {
        std::fprintf(stderr, "--captures and --report-out are required\n");
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) return 2;

    fs::create_directories(args.report_out);
    fs::create_directories(fs::path(args.report_out) / "diffs");

    StageStats stats[6];
    for (int i = 0; i < 6; ++i) {
        stats[i].id = kStages[i].id;
        stats[i].is_stub = kStages[i].is_stub;
        stats[i].diffs_dir = fs::path(args.report_out) / "diffs";
        stats[i].max_diffs = args.max_diffs_per_stage;
    }

    std::vector<fs::path> caps;
    for (const auto& e : fs::directory_iterator(args.captures)) {
        if (e.is_regular_file() && e.path().extension() == ".cap") {
            caps.push_back(e.path());
        }
    }
    std::sort(caps.begin(), caps.end());

    std::vector<uint8_t> out_buf;
    for (const auto& cap_path : caps) {
        CaptureReader reader;
        if (!reader.open(cap_path.string().c_str())) {
            std::fprintf(stderr, "skip unreadable capture: %s\n", cap_path.string().c_str());
            continue;
        }

        for (uint32_t idx = 0; idx < reader.stage_count(); ++idx) {
            StageRecord rec;
            if (!reader.read_stage(idx, &rec)) continue;
            int sidx = static_cast<int>(rec.stage_id) - 1;
            if (sidx < 0 || sidx >= 6) continue;
            if ((args.stage_mask & (1u << sidx)) == 0) continue;

            const StageEntry& entry = kStages[sidx];
            out_buf.assign(entry.out_buf_cap, 0);
            size_t out_len = 0;
            bool ok = entry.fn(rec.in_bytes, rec.in_len,
                               out_buf.data(), out_buf.size(), &out_len);
            stats[sidx].record_one(reader.hash_seq(), rec.in_len, rec.out_len,
                                   ok, out_len, out_buf.data(), rec.out_bytes,
                                   rec.in_bytes);
        }
    }

    write_report(args.report_out, args.captures, stats, 6);
    return 0;
}
