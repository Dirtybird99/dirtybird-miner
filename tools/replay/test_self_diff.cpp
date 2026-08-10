// test_self_diff.cpp - Clone-vs-clone replay harness canary.

#include "cap_format.h"
#include "../../include/dluna_stages.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace deroluna::replay;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_failed++; } } while (0)

static bool synth_stage(StageId id,
                        std::vector<uint8_t>* in,
                        std::vector<uint8_t>* out) {
    using namespace deroluna::stages;
    static std::mt19937 rng(0xCAFE);
    auto fill = [&](std::vector<uint8_t>& v, size_t n) {
        v.resize(n);
        for (auto& b : v) b = static_cast<uint8_t>(rng() & 0xFF);
    };

    if (id == StageId::Salsa20Init) {
        fill(*in, 76);
        out->assign(256, 0);
        size_t out_len = 0;
        bool ok = stage_salsa20_init(in->data(), in->size(),
                                     out->data(), out->size(), &out_len);
        out->resize(out_len);
        return ok;
    }
    if (id == StageId::Rc4Ksa) {
        fill(*in, 256);
        out->assign(256, 0);
        size_t out_len = 0;
        bool ok = stage_rc4_ksa(in->data(), in->size(),
                                out->data(), out->size(), &out_len);
        out->resize(out_len);
        return ok;
    }
    if (id == StageId::BranchDispatch) {
        const uint32_t buf_len = 4096;
        in->assign(4 + buf_len, 0);
        std::memcpy(in->data(), &buf_len, sizeof(buf_len));
        for (uint32_t i = 0; i < buf_len; ++i) {
            (*in)[4 + i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        out->assign(200000, 0);
        size_t out_len = 0;
        bool ok = stage_branch_dispatch(in->data(), in->size(),
                                        out->data(), out->size(), &out_len);
        out->resize(out_len);
        return ok;
    }
    if (id == StageId::Sha256OfSa) {
        fill(*in, 70042 * 4);
        out->assign(32, 0);
        size_t out_len = 0;
        bool ok = stage_sha256_of_sa(in->data(), in->size(),
                                     out->data(), out->size(), &out_len);
        out->resize(out_len);
        return ok;
    }
    return false;
}

static uint64_t json_num(const std::string& line, const char* key) {
    size_t pos = line.find(key);
    if (pos == std::string::npos) return 0;
    pos = line.find(':', pos);
    if (pos == std::string::npos) return 0;
    return std::strtoull(line.c_str() + pos + 1, nullptr, 10);
}

int main() {
    fs::path tmp = fs::temp_directory_path() /
        ("dlcap_self_diff_" + std::to_string(std::rand()));
    fs::path caps = tmp / "captures";
    fs::path out = tmp / "report";
    fs::create_directories(caps);
    fs::create_directories(out);

    for (int hash_seq = 0; hash_seq < 8; ++hash_seq) {
        CaptureWriter writer;
        fs::path cap_path = caps / (std::to_string(hash_seq) + ".cap");
        CHECK(writer.open(cap_path.string().c_str(), static_cast<uint64_t>(hash_seq)));
        for (int s = 1; s <= 6; ++s) {
            std::vector<uint8_t> in_buf;
            std::vector<uint8_t> out_buf;
            if (!synth_stage(static_cast<StageId>(s), &in_buf, &out_buf)) {
                continue;
            }
            CHECK(writer.write_stage(static_cast<StageId>(s),
                                     in_buf.data(), in_buf.size(),
                                     out_buf.data(), out_buf.size()));
        }
        CHECK(writer.close());
    }

#ifdef _WIN32
    std::string exe = ".\\dirtybird-c-miner-replay.exe";
#else
    std::string exe = "./dirtybird-c-miner-replay";
#endif
    std::string cmd = exe + " --captures \"" + caps.string() +
                      "\" --report-out \"" + out.string() + "\"";
    int rc = std::system(cmd.c_str());
    CHECK(rc == 0);

    std::ifstream jsonl(out / "report.jsonl");
    CHECK(jsonl.is_open());
    std::string line;
    uint64_t rows = 0;
    while (std::getline(jsonl, line)) {
        if (line.find("\"is_stub\":true") != std::string::npos) continue;
        if (line.find("\"hash_seq\"") == std::string::npos ||
            line.find("\"match\":true") == std::string::npos ||
            line.find("\"coverage\"") != std::string::npos) {
            std::fprintf(stderr, "FAIL self-diff: %s\n", line.c_str());
            g_failed++;
        }
        rows++;
    }
    CHECK(rows == 8 * 4);
    jsonl.close();

    fs::remove_all(tmp);
    if (g_failed) {
        std::fprintf(stderr, "%d failures\n", g_failed);
        return 1;
    }
    std::printf("test_self_diff: all OK\n");
    return 0;
}
