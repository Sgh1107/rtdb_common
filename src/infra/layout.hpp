#ifndef RTDB_SRC_INFRA_LAYOUT_HPP_
#define RTDB_SRC_INFRA_LAYOUT_HPP_

#include <atomic>
#include <cstdint>

#include "rtdb/version.hpp"

namespace rtdb {
namespace layout {

/// "RTDBCMN1" 的内存表示（小端机器上 ASCII 倒序）。
inline constexpr uint64_t kShmMagic = 0x52544442434D4E31ULL;

/// 共享内存头部保留区大小：SuperBlock 所在区域。
inline constexpr uint64_t kHeaderReservedBytes = 4096;

// ---- 固定元数据分区（由本文件统一约定，不进 SuperBlock 字段）----
/// 分配器控制块偏移/大小。
inline constexpr uint64_t kAllocCtlOffset = 4096;
inline constexpr uint64_t kAllocCtlBytes = 512;
/// 根互斥锁状态字偏移/大小。
inline constexpr uint64_t kRootMutexOffset = 4608;
inline constexpr uint64_t kRootMutexBytes = 64;
/// 可分配数据区的起始偏移（页对齐）。
inline constexpr uint64_t kDataStartOffset = 8192;

/// 共享内存超级块（docs/02 §3）。
///
/// 规则：
///  - 必须保持标准布局（standard-layout）、定长、无虚函数/裸指针；
///  - [稳定字段] 参与 layout_crc 计算，调整顺序或增删必须递增
///    kShmFormatVersion 并同步迁移工具；
///  - [运行时字段] 由 std::atomic 视角读写（C++17 无 atomic_ref，
///    以 reinterpret_cast 到等宽 atomic 的惯用法实现，见下方 helper；
///    要求平台为无锁 64 位原子——x86_64/ARM64 均满足）；
///  - 新建时全部清零后填充。
struct shm_SuperBlock {
    // ==================== 稳定字段（参与 CRC）====================
    uint64_t magic;           ///< 固定 kShmMagic
    uint32_t format_version;  ///< kShmFormatVersion
    uint32_t header_size;     ///< sizeof(shm_SuperBlock)，防结构膨胀错读
    uint32_t pointer_width;   ///< sizeof(void*)，拦截跨字宽误挂载
    uint32_t layout_crc;      ///< 稳定字段区 CRC（计算时自身按 0 计）
    int64_t created_unix_ns;  ///< 映像创建时间（Unix 纳秒）
    uint64_t max_size_bytes;  ///< 映像总容量

    // ==================== 运行时字段（不参与 CRC）====================
    /// 已提交变更的全局最大序列号（int64 单调递增，M1 WAL 接管推进权）。
    uint64_t committed_seq;
    /// 主写者心跳（steady 纳秒时间戳；0 表示当前无活跃写者）。
    uint64_t writer_heartbeat_ns;
    /// 成功 Open 的累计次数（会话计数器，活性观测用）。
    int64_t session_count;
    /// Role::XXX 位标志持久化记录。
    uint32_t role_flags;
    uint32_t fsync_policy;

    char reserved[4096 - 6 * 8 - 6 * 4];  ///< 补齐至整 4096 字节
};

static_assert(sizeof(shm_SuperBlock) == kHeaderReservedBytes,
              "shm_SuperBlock 必须恰好占满头部保留区");
static_assert(std::is_standard_layout<shm_SuperBlock>::value, "shm_SuperBlock 必须是标准布局");

// ---- 64 位运行时字段的原子访问 helper ----
static_assert(sizeof(std::atomic<int64_t>) == sizeof(int64_t), "需要无锁 64 位原子支持");

inline std::atomic<int64_t>* AsAtomicI64(int64_t* p) noexcept {
    return reinterpret_cast<std::atomic<int64_t>*>(p);
}
inline std::atomic<uint64_t>* AsAtomicU64(uint64_t* p) noexcept {
    return reinterpret_cast<std::atomic<uint64_t>*>(p);
}

/// SuperBlock 所在地址即映射基址偏移 0。
inline shm_SuperBlock* SuperBlockAt(void* base) noexcept {
    return static_cast<shm_SuperBlock*>(base);
}

}  // namespace layout
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_LAYOUT_HPP_
