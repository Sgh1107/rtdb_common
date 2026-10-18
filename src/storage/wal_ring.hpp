#ifndef RTDB_SRC_STORAGE_WAL_RING_HPP_
#define RTDB_SRC_STORAGE_WAL_RING_HPP_

/// \file
/// \brief 映像内环形 WAL（WalRing，docs/03 §3.2，docs/08 §2-D1/D2/D3，§3-W4）。
///
/// 物理结构（单次 Slab 分配，偏移写入 SuperBlock.wal_ring_off/wal_ring_bytes）：
/// ```
/// [shm_WalRingHeader 64B]
/// [记录区 area_bytes]  ← 变长记录顺序写，写满回绕
/// ```
///
/// 记录线格式（D3 铁律：逐字段小端显式编码，禁止整 struct 落盘）：
/// ```
/// [0]  u32 total_len        整条记录字节数（逐字节紧排，含 CRC）
/// [4]  u32 payload_len
/// [8]  u64 seq              全局单调序号（pad 记录恒为 0）
/// [16] u64 ts_ns            写入时刻（Unix 纳秒）
/// [24] u64 row_no
/// [32] u64 row_ver          应用完成后的行版本（幂等重放判重依据）
/// [40] i64 table_id
/// [48] u8  op
/// [49] u8  flags            预留（恒 0）
/// [50] u16 reserved
/// [52] u32 reserved2
/// [56] payload[payload_len]
/// [total_len-4] u32 crc     Crc32(记录首部至 CRC 前)
/// ```
/// 头部 56B + 载荷 + CRC 4B，逐字节紧排（不做对齐填充）；docs/08 §2-D3 的
/// "16B 头"落地为 56B 定长头（全部控制字段内联，落地差异已记录 docs/08 §3-W4）。
///
/// 回绕协议：
///  - 记录尾部剩余空间 < kWalMinRecordBytes 时不写任何内容直接回绕
///    （扫描方对 remaining < 60B 一律回绕跳过）；
///  - 剩余空间 ≥ 60B 但容不下当前记录时写一条 kOpPad 填充记录（占满至
///    区尾），随后从头继续；扫描方按 total_len 跳过 pad；
///  - **覆盖前先落盘**（D1）：环空间不足时由调用方（Engine 写路径）强制
///    组刷全映像、推进 checkpoint_seq 后调用 `DropAllRetained()` 丢弃全部
///    保留记录——保证任意时刻 (checkpoint_seq, committed_seq] 重放区间
///    在环内完整可用。
///
/// 毒化语义（D2）：追加失败/校验失败后置 poisoned，引擎此后拒绝一切写
/// （kPoisoned），读不受影响。毒化标志持久化于环头，跨进程/重启可见。
///
/// 并发约定：全部变更须持 ShmRootMutex（单写者）；扫描仅在打开恢复路径
/// （无并发写者）执行。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>

#include "infra/layout.hpp"
#include "infra/slab_allocator.hpp"
#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace storage {

class Catalog;

/// 环头魔数（"RTDBWAL1"）。
inline constexpr uint64_t kWalRingMagic = 0x5254444257414C31ULL;

/// 新建环的 Slab 块容量（含 64B 环头；恰为 slab 最大级 64KB）。
inline constexpr uint64_t kDefaultWalRingBytes = 64 * 1024;
/// 记录区最小容量（创建校验）。
inline constexpr uint64_t kMinWalAreaBytes = 4096;

/// 记录线格式常量。
inline constexpr uint32_t kWalRecHdrBytes = 56;     ///< 定长头
inline constexpr uint32_t kWalRecCrcBytes = 4;      ///< CRC 尾
inline constexpr uint32_t kWalMinRecordBytes = 60;  ///< 最小记录（56+0+4）

/// WAL 操作码（持久化取值，只增不改）。
enum WalOp : uint8_t {
    kWalOpInsert = 1,      ///< 插入行；payload = 整行记录（rec_fixed_len 字节）
    kWalOpUpdate = 2,      ///< 整行覆盖更新；payload 同上
    kWalOpDelete = 3,      ///< 删除行；payload = 主键有序编码（8B）
    kWalOpBatchBegin = 4,  ///< 原子批开始；payload = u32 LE 操作数
    kWalOpBatchCommit = 5, ///< 原子批提交标记；payload 为空
    kWalOpCreateTable = 6, ///< 建表（M1 仅记录；重放还原归 W5/M2）
    kWalOpDropTable = 7,   ///< 删表（同上）
    kWalOpPad = 0xFF,      ///< 回绕填充（无 seq，扫描方跳过）
};

// ---------------- 小端显式编解码（WAL 线格式共用，D3 铁律）----------------

inline void EncodeU32LE(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void EncodeU64LE(uint8_t* dst, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

inline uint32_t DecodeU32LE(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

inline uint64_t DecodeU64LE(const uint8_t* src) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | src[i];
    return v;
}

/// 环头（64B 定长，标准布局）。
///
/// 并发访问约定：
///  - magic/area_bytes/head_seq/tail_seq/head_pos/tail_pos：单写者（持
///    ShmRootMutex 的写线程）变更，恢复路径（无并发写者）经诊断方法读写，
///    无需原子；
///  - poisoned：写线程置位、业务线程在根锁之外读取（拒写判定）→ 必须走
///    原子视角（AsAtomicU32），否则构成数据竞争（TSan 报告）。
struct shm_WalRingHeader {
    uint64_t magic;      ///< 固定 kWalRingMagic
    uint64_t area_bytes; ///< 记录区容量（不含本头）
    uint64_t head_seq;   ///< 下一个待分配 seq（从 1 起，单调不回退）
    uint64_t tail_seq;   ///< 环内最老保留记录的 seq（空环 = head_seq）
    uint64_t head_pos;   ///< 下一条记录写入位置（记录区内偏移，< area_bytes）
    uint64_t tail_pos;   ///< 最老保留记录位置
    uint32_t poisoned;   ///< 0/1：毒化后拒写保读（原子访问：AsAtomicU32）
    uint32_t reserved0;
    uint64_t reserved[1];
};
static_assert(sizeof(shm_WalRingHeader) == 64, "环头必须恰好 64 字节");
static_assert(std::is_standard_layout<shm_WalRingHeader>::value, "环头必须是标准布局");

// ---- 共享内存运行时字段的原子视角 helper（C++17 无 atomic_ref，reinterpret_cast
//      到等宽 atomic 的惯用法；与 layout::AsAtomicU64 同款）。----
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "需要无锁 32 位原子支持");

/// poisoned 字段（写线程置位、其他线程无根锁读取）的原子视图。
inline std::atomic<uint32_t>* AsAtomicU32(uint32_t* p) noexcept {
    return reinterpret_cast<std::atomic<uint32_t>*>(p);
}

/// \brief 扫描交付的记录只读视图（载荷指针指向环内，扫描期间有效）。
struct WalRecordView {
    uint64_t seq;
    uint64_t ts_ns;
    uint64_t row_no;
    uint64_t row_ver;
    int64_t table_id;
    uint8_t op;
    const uint8_t* payload;  ///< 不为空时长度 payload_len（可为 0）
    uint32_t payload_len;
    /// 记录在记录区内的归一化结束位置：记录字节末尾，恰为区尾时归一化为 0
    /// （与 hdr.head_pos 同语义）。仅恢复路径（ReplayWal）用于回绕写头——
    /// 有效前缀的最后一条记录结束后即“下一次 Append 应写的 head_pos”。
    uint64_t end_pos;
};

/// \brief 映像内环形 WAL。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)  // std::function 成员，PImpl 同款预期内用法
#endif
class RTDB_API WalRing {
public:
    using RecordFn = std::function<Err(const WalRecordView&)>;

    /// 附着既有环；映像未布局环时（SuperBlock.wal_ring_off == 0）从 alloc
    /// 分配并初始化（写回 SuperBlock 与 layout CRC）。`out_created` 返回
    /// 是否为新创建。附着校验：魔数、tail_seq ≤ checkpoint_seq + 1
    /// （覆盖协议不变量）、seq/位置自洽；违例返回 kCorruption。
    static Result<std::unique_ptr<WalRing>> CreateOrAttach(void* base_addr,
                                                           infra::SlabShmAllocator* alloc,
                                                           layout::shm_SuperBlock* sb,
                                                           bool* out_created = nullptr);

    bool poisoned() const noexcept {
        return AsAtomicU32(&hdr_->poisoned)->load() != 0;
    }
    /// 毒化（持久化；拒写保读的开关，docs/08 §2-D2）。
    void MarkPoisoned() noexcept { AsAtomicU32(&hdr_->poisoned)->store(1); }

    uint64_t next_seq() const noexcept { return hdr_->head_seq; }    ///< 下一个待分配 seq
    uint64_t oldest_seq() const noexcept { return hdr_->tail_seq; }  ///< 最老保留 seq

    /// \brief 记录区容量（不含 64B 环头；批前预留/容量诊断用）。
    uint64_t area_bytes() const noexcept { return area_bytes_; }

    /// \brief 记录占用的总字节数（含头部/CRC/8 对齐填充）。
    static uint64_t RecordBytes(uint32_t payload_len) noexcept;

    /// \brief 环内剩余可写空间能否容纳一条 consumed = RecordBytes(len) 的记录。
    ///
    /// 精确判据（P0-3 修复）：除总量足够外，还须满足回绕落位约束——当
    /// [head_pos, area) 的连续空间不足时，写方垫片回绕后从 0 写记录，此时
    /// 记录须整体容身 [0, tail_pos)；否则垫片回绕会覆盖 [tail_pos, head_pos)
    /// 的环内数据。原判据只看总量，在该窗口存在静默覆盖。
    bool HasRoomFor(uint64_t rec_bytes) const noexcept;

    /// \brief 追加一条记录（调用方须持根锁，且此前已确认 HasRoomFor）。
    ///
    /// 回绕处理（pad/裸回绕）内部完成；空间判据与 HasRoomFor 一致。
    /// 内部注入点：`wal.append.mid`（头部已写、载荷未写 → 撕裂记录，
    /// 供崩溃注入用例）。
    ///
    /// \param out_seq 可选输出：分配到的 seq
    /// \return kOk / kPoisoned / kLimitExceeded（记录大于整环或未确认空间）
    Err Append(uint8_t op, int64_t table_id, uint64_t row_no, uint64_t row_ver,
               const uint8_t* payload, uint32_t payload_len, uint64_t* out_seq) noexcept;

    /// \brief 追加批标记记录（kWalOpBatchBegin / kWalOpBatchCommit 的便捷封装）。
    Err AppendMark(uint8_t op, const uint8_t* payload, uint32_t payload_len,
                   uint64_t* out_seq) noexcept;

    /// \brief 丢弃全部保留记录（tail := head）。
    ///
    /// 前置条件（调用方保证）：checkpoint_seq ≥ 全部保留记录的 seq，
    /// 即被丢弃记录已全部应用且落盘，重放不再需要它们。
    void DropAllRetained() noexcept;

    /// \brief 恢复期回绕写头（**仅** ReplayWal 单线程恢复路径调用：不得持
    /// 根锁、不得有并发写者/扫描者）。
    ///
    /// 崩溃可能把写头留在“已分配但未 COMMIT”的后缀之后（撕裂批：BEGIN 已
    /// 落环、COMMIT 未落环）。若恢复只发布 committed_seq 而不回退写头，
    /// 下一次 Append 会从旧的 head_seq 继续分配，跳过的 seq 区间（被丢弃
    /// 批内记录）此前已完整落环且 CRC 有效；一旦新记录提交、committed_seq
    /// 跨过该区间，ScanRange/ChangeCursor 就会把从未 COMMIT 的记录当作变更
    /// 事件交付（违 docs/13 §3-D1“seq ≤ committed 的记录所在批必已 COMMIT”）。
    /// 本方法把写头精确回退到有效前缀末尾，使 seq 连续：下一次 Append 分配
    /// 到的 seq == next_seq，不会产生幻影区间。
    ///
    /// 允许把环退回逻辑空环（next_seq == tail_seq），也可缩小 PendingBytes。
    ///
    /// \param next_seq 回退后的 hdr.head_seq（须 tail_seq ≤ next_seq ≤ head_seq）
    /// \param next_pos 回退后的 hdr.head_pos（有效前缀末记录的归一化 end_pos，
    ///                 须 < area_bytes）
    /// \return kOk / kUnsupported（未附着）/ kCorruption（参数越界）
    Err RewindHeadAfterReplay(uint64_t next_seq, uint64_t next_pos) noexcept;

    /// \brief 空环回退：hdr.head := hdr.tail（恢复扫描未发现任何有效前缀时用；
    /// 同样仅限 ReplayWal 单线程恢复路径）。
    void RewindHeadToTail() noexcept;

    /// \brief 从 tail_pos 起顺序扫描有效记录（CRC + seq 连续性校验）。
    ///
    /// 撕裂尾部是崩溃的正常痕迹：停止扫描并返回 kOk（`out_last_seq` =
    /// 最后一条交付记录的 seq，0 = 无）。撕裂的 pad 记录若 total_len 完好
    /// 则跳过（回绕后记录仍可达）。回调返回非 kOk 立即中止扫描并透传。
    /// 结构性违例（seq 断档等）返回 kCorruption。
    ///
    /// \param out_last_seq 可选输出：最后交付的 seq
    Err ScanValid(const RecordFn& fn, uint64_t* out_last_seq) const;

    /// \brief 增量扫描交付回调（订阅路径）：返回 false = 请求正常停止，
    /// 当前记录**不**计入交付（调用方可从该记录重扫，无缺漏）。
    using ScanRangeFn = std::function<bool(const WalRecordView&)>;

    /// \brief 从 from_seq（不含）扫描至 to_seq（含）——订阅增量读取面
    /// （docs/13 §3-D1）。
    ///
    /// 调用方须持根锁（与覆盖丢弃互斥，保证扫描窗口内容稳定）。
    /// 基础语义与 ScanValid 对齐（CRC/连续性校验、pad 跳过、撕裂尾部
    /// 视为正常终点）；增量语义额外约定：
    ///  - seq ≤ from_seq 的记录仅做边界/连续性校验，不交付；
    ///  - 考察到 seq > to_seq 或回调返回 false 即停（均属正常返回）；
    ///  - `from_seq + 1 < oldest_seq()`（区间起点已被覆盖回收）返回
    ///    kCursorOverflow：调用方须全量重取后重置游标；
    ///  - 空环/区间为空/起点晚于已写入上限均返回 kOk（out_last_seq =
    ///    from_seq）。
    /// \param out_last_seq 最后交付的 seq（fn 接受的推进；无 = from_seq）
    Err ScanRange(uint64_t from_seq_excl, uint64_t to_seq_incl, const ScanRangeFn& fn,
                  uint64_t* out_last_seq) const;

private:
    WalRing() = default;

    uint8_t* Area() const noexcept;
    /// tail_pos → head_pos 的逻辑占用字节数（0 = 空环）。
    uint64_t PendingBytes() const noexcept;
    /// 写一条占满 [head_pos, area_bytes) 的 pad 记录并回绕。
    Err WriteTailPadAndWrap() noexcept;

    shm_WalRingHeader* hdr_ = nullptr;
    uint8_t* area_ = nullptr;
    uint64_t area_bytes_ = 0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// \brief 恢复重放统计（诊断/测试用）。
struct ReplayStats {
    uint64_t records_walked = 0;    ///< 扫描到的有效记录数（不含 pad/批标记）
    uint64_t records_applied = 0;   ///< 实际重放应用的记录数（seq > checkpoint）
    uint64_t batches_discarded = 0; ///< 因无 COMMIT 而整批作废的批数（0/1）
    uint64_t recovered_to_seq = 0;  ///< 重放后发布的 committed_seq
    /// 是否回绕了写头（丢弃未提交后缀；head_seq 实际发生变化时为 true）。
    /// 崩溃撕裂批/半批后应恒为 true，正常重开为 false。
    bool head_rewound = false;
};

/// \brief 恢复路径 v1（docs/08 §3-W4）：扫描 → 批过滤 → 幂等重放 → 回绕写头 → 发布。
///
/// 在 Engine::Open（写侧角色）上执行，root 锁**未持有**（打开路径无并发
/// 写者）：
///  1. `ScanValid` 顺序扫描；BATCH_BEGIN 与 BATCH_COMMIT 之间的记录缓存，
///     见到 COMMIT 才应用；扫描结束仍处于批内（无 COMMIT）→ 整批作废
///     （`kBatchAborted` 语义，见 docs/08 §2-D3）；
///  2. 逐条幂等重放（seq > checkpoint_seq 才应用）：Insert/Update 按
///     row_no + row_ver 判重后整行重写并补发布索引；Delete 补墓碑并摘键；
///     DDL 记录（Create/DropTable）v1 跳过（目录重建归 W5/M2）；
///  3. 触及表重建 committed_row_count（= 索引规模）；
///  4. **回绕写头**：若 head_seq 越过有效前缀末尾（崩溃遗留未提交后缀），
///     把写头退到有效前缀末记录的 end_pos（无有效前缀时退到 tail）；此后
///     next Append 的 seq == 有效前缀末 seq + 1，恢复 seq 连续，杜绝
///     “committed_seq 跳过从未 COMMIT 的 seq 区间”的幻影变更交付；
///  5. 发布 committed_seq = max(最后一条完整记录 seq（未提交批之前）,
///     checkpoint_seq)。
///
/// \param out_stats 可选输出
/// \return kOk / kCorruption / kOutOfMemory（重放申请空间失败）
Err ReplayWal(Catalog* catalog, void* base_addr, infra::SlabShmAllocator* alloc, WalRing* ring,
              layout::shm_SuperBlock* sb, ReplayStats* out_stats);

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_WAL_RING_HPP_
