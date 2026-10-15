#include "storage/group_commit.hpp"

#include <chrono>

#include "infra/shm_region.hpp"

namespace rtdb {
namespace storage {

using layout::AsAtomicU64;

GroupCommit::GroupCommit(ShmRegion* region, layout::shm_SuperBlock* sb, FsyncPolicy policy)
    : region_(region), sb_(sb), policy_(policy) {
    flushed_ = AsAtomicU64(&sb_->flushed_seq)->load();
    if (policy_ == FsyncPolicy::PerSecond) {
        worker_ = std::thread([this] { BackgroundLoop(); });
    }
}

GroupCommit::~GroupCommit() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Err GroupCommit::FlushNow(uint64_t* out_covered) noexcept {
    // 水位快照必须在 flush 发起之前读取：flush 只保证调用时已脏的页落盘，
    // 期间新提交的页不被承诺（只可能少记不可能多记）。
    const uint64_t target = AsAtomicU64(&sb_->committed_seq)->load();
    const Err e = region_->FlushAll();
    if (IsOk(e)) {
        auto* fs = AsAtomicU64(&sb_->flushed_seq);
        uint64_t cur = fs->load();
        while (cur < target && !fs->compare_exchange_weak(cur, target)) {
        }
    }
    if (out_covered != nullptr) *out_covered = IsOk(e) ? target : 0;
    return e;
}

Err GroupCommit::FlushThrough(uint64_t seq, uint32_t timeout_ms) noexcept {
    std::unique_lock<std::mutex> lk(mu_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (flushed_ < seq) {
        if (flush_error_ != Err::kOk) {
            // 上一次 flush 失败：分发给等待者并清除，由新 leader 重试。
            const Err e = flush_error_;
            flush_error_ = Err::kOk;
            cv_.notify_all();
            return e;
        }
        if (!flushing_) {
            flushing_ = true;
            lk.unlock();
            uint64_t covered = 0;
            const Err e = FlushNow(&covered);
            lk.lock();
            flushing_ = false;
            if (IsOk(e)) {
                if (covered > flushed_) flushed_ = covered;
            } else {
                flush_error_ = e;
            }
            cv_.notify_all();
            continue;
        }
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) return Err::kTimeout;
    }
    return Err::kOk;
}

Err GroupCommit::OnCommit(uint64_t seq) noexcept {
    switch (policy_) {
        case FsyncPolicy::Always:
            return FlushThrough(seq);
        case FsyncPolicy::PerSecond:
            cv_.notify_all();  // 唤醒后台节流线程
            return Err::kOk;
        case FsyncPolicy::Never:
        default:
            return Err::kOk;
    }
}

uint64_t GroupCommit::flushed_seq() const noexcept {
    return AsAtomicU64(&sb_->flushed_seq)->load();
}

void GroupCommit::BackgroundLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait_for(lk, std::chrono::milliseconds(kFlushIntervalMs),
                     [this] { return stop_; });
        if (stop_) break;
        if (flushing_) continue;  // leader 在刷，本轮跳过
        // 无新提交时也照刷（幂等、廉价），保持 ≤1s 窗口语义简单可靠。
        flushing_ = true;
        lk.unlock();
        uint64_t covered = 0;
        const Err e = FlushNow(&covered);
        lk.lock();
        flushing_ = false;
        if (IsOk(e)) {
            if (covered > flushed_) flushed_ = covered;
        }
        cv_.notify_all();
    }
}

}  // namespace storage
}  // namespace rtdb
