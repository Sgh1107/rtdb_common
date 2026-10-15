/// W4 诊断：打开 kill -9 崩溃映像，转储恢复后父进程视角（committed_seq /
/// 行可见性），用于定位恢复重放类失败。用法：dbg_repro <image_path>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "rtdb/engine.hpp"

namespace {

const char* ErrName(rtdb::Err e) {
    switch (e) {
        case rtdb::Err::kOk: return "kOk";
        case rtdb::Err::kNotFound: return "kNotFound";
        case rtdb::Err::kAlreadyExists: return "kAlreadyExists";
        case rtdb::Err::kCorruption: return "kCorruption";
        case rtdb::Err::kPoisoned: return "kPoisoned";
        case rtdb::Err::kIoError: return "kIoError";
        case rtdb::Err::kOutOfMemory: return "kOutOfMemory";
        case rtdb::Err::kTimeout: return "kTimeout";
        case rtdb::Err::kInvalidArgument: return "kInvalidArgument";
        case rtdb::Err::kUnsupported: return "kUnsupported";
        default: return "kOther";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dbg_repro <image_path>\n");
        return 1;
    }
    const std::string image = argv[1];

    rtdb::Options opts;
    opts.instance_name = "dbg_repro";
    opts.file_path = image;
    opts.initial_size_bytes = 2ull << 20;
    opts.role = rtdb::Role::Standalone;
    opts.fsync = rtdb::FsyncPolicy::Always;

    auto open_res = rtdb::Engine::Open(opts);
    if (!open_res.IsOk()) {
        std::printf("OPEN FAILED err=%s\n", ErrName(open_res.Error()));
        return 2;
    }
    auto eng = open_res.TakeValue();
    std::printf("committed_seq=%lld flushed_seq=%lld\n",
                static_cast<long long>(eng->CommittedSeq()),
                static_cast<long long>(eng->FlushedSeq()));

    auto t_res = eng->OpenTable("t");
    if (!t_res.IsOk()) {
        std::printf("OpenTable FAILED err=%s\n", ErrName(t_res.Error()));
        return 3;
    }
    const rtdb::TableHandle t = t_res.Value();
    std::printf("table: id=%lld row_cap=%u\n", static_cast<long long>(t.table_id()),
                static_cast<unsigned>(t.row_cap()));

    for (int64_t k = 1; k <= 5; ++k) {
        auto g = eng->GetByKey(t, rtdb::ColumnValue::Int(k));
        if (g.IsOk())
            std::printf("  key=%lld OK val=%lld\n", static_cast<long long>(k),
                        static_cast<long long>(g.Value()[1].as_int()));
        else
            std::printf("  key=%lld %s\n", static_cast<long long>(k), ErrName(g.Error()));
    }
    for (int64_t k = 100; k <= 105; ++k) {
        auto g = eng->GetByKey(t, rtdb::ColumnValue::Int(k));
        if (g.IsOk())
            std::printf("  key=%lld OK val=%lld\n", static_cast<long long>(k),
                        static_cast<long long>(g.Value()[1].as_int()));
        else
            std::printf("  key=%lld %s\n", static_cast<long long>(k), ErrName(g.Error()));
    }
    return 0;
}
