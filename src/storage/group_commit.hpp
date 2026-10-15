#ifndef RTDB_SRC_STORAGE_GROUP_COMMIT_HPP_
#define RTDB_SRC_STORAGE_GROUP_COMMIT_HPP_

/// \file
/// \brief 组提交协调器（docs/03 §3.2，docs/08 §2-D1/D2，§3-W4）。
///
/// 职责：
///  - **always 档**：提交返回前完成全映像 flush（leader/follower 聚合——
///    flush 进行期间到达的提交作为 follower 等待同一次 flush 完成），
///    已确认提交断电零丢失；
///  - **per_second 档（默认）**：提交即返回（写入页缓存），后台线程按
///    kFlushIntervalMs 节流全映像 flush，断电丢最近一个窗口（进程崩溃
///    不受影响——mmap 共享页与文件背衬一致，docs/08 §2-D1）；
///  - **never 档**：不主动 flush（仅进程内一致；优雅关闭时仍收尾一次）。
///
/// flush 粒度为**全映像**（头部 + WAL 环 + 数据页），与 D1 的
/// "本组提交已 flush 全映像" 承诺一致；flushed_seq 账本取 flush 发起前
/// 的 committed_seq 快照（只可能少记不可能多记）。
///
/// 线程模型：写路径在**根锁释放后**调用 `OnCommit`（flush 不持根锁，
/// 流水线化热路径）；后台线程仅做 flush 与水位账本，不取根锁。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "infra/layout.hpp"
#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"

namespace rtdb {

class ShmRegion;

namespace storage {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)  // std::mutex/cv/thread 成员，PImpl 同款预期内用法
#endif

/// \brief 全映像组提交协调器（宿主侧对象，随 Engine 实例生命周期）。
class RTDB_API GroupCommit {
public:
    /// per_second 档后台 flush 间隔。
    static constexpr uint32_t kFlushIntervalMs = 1000;
    /// FlushThrough 默认等待上限。
    static constexpr uint32_t kDefaultFlushTimeoutMs = 5000;

    /// \param region 全映像 flush 载体（须非空）
    /// \param sb flushed_seq 账本所在超级块（须非空）
    /// \param policy 三档持久化语义；per_second 启动后台节流线程
    GroupCommit(ShmRegion* region, layout::shm_SuperBlock* sb, FsyncPolicy policy);
    ~GroupCommit();  ///< 停止后台线程（不做收尾 flush，见 Engine 析构）

    GroupCommit(const GroupCommit&) = delete;
    GroupCommit& operator=(const GroupCommit&) = delete;

    /// \brief 写路径提交点（根锁释放后调用）。
    /// \return always 档透传 FlushThrough 结果；其余档位恒 kOk
    Err OnCommit(uint64_t seq) noexcept;

    /// \brief 组提交：阻塞直至 flushed_seq ≥ seq（leader/follower 聚合）。
    /// \return kOk / kTimeout / kIoError
    Err FlushThrough(uint64_t seq, uint32_t timeout_ms = kDefaultFlushTimeoutMs) noexcept;

    /// \brief 执行一次全映像 flush；`out_covered` 返回本次覆盖到的
    /// committed_seq 快照（在 flush 发起前读取），并推进 sb->flushed_seq。
    Err FlushNow(uint64_t* out_covered) noexcept;

    uint64_t flushed_seq() const noexcept;

private:
    void BackgroundLoop();

    ShmRegion* region_;
    layout::shm_SuperBlock* sb_;
    FsyncPolicy policy_;

    std::mutex mu_;
    std::condition_variable cv_;
    bool flushing_ = false;   ///< 有 leader 正在 flush
    bool stop_ = false;        ///< 后台线程退出标记
    Err flush_error_ = Err::kOk;  ///< 最近一次 flush 失败码（等待者分发）
    uint64_t flushed_ = 0;     ///< 宿主侧水位镜像（≥ 才视为持久）
    std::thread worker_;       ///< per_second 档后台线程（可能未启动）
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_GROUP_COMMIT_HPP_
