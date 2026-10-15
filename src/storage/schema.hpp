#ifndef RTDB_SRC_STORAGE_SCHEMA_HPP_
#define RTDB_SRC_STORAGE_SCHEMA_HPP_

/// \file 表 schema 的共享内存定长结构与行布局计算（docs/03 §3.1，docs/08 §3-W1）。
///
/// 约定：
///  - 本头文件全部为 shm_ 前缀的标准布局结构（驻留映像）与布局计算纯函数，
///    不含任何成员函数/虚函数/裸指针/std 容器；
///  - 映像内结构体靠 pointer_width 校验与同机字长保障可读性，不做字节序
///    序列化（WAL 记录才做逐字段小端编码，见 docs/08 §2-D3）；
///  - 宿主侧类型（`rtdb::DataType` / `ColumnDef` / `TableDef`）定义在公共头
///    `rtdb/table_def.hpp`，本文件仅引用。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rtdb/err.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {
namespace storage {

/// 定长类型的字节宽度；变长类型返回 0（内容在变长池）。
constexpr uint32_t FixedSizeOf(DataType t) noexcept {
    switch (t) {
        case DataType::kInt8:
            return 1;
        case DataType::kInt16:
            return 2;
        case DataType::kInt32:
            return 4;
        case DataType::kInt64:
            return 8;
        case DataType::kFloat32:
            return 4;
        case DataType::kFloat64:
            return 8;
        case DataType::kBool:
            return 1;
        case DataType::kUtf8Var:
            return 0;
        case DataType::kBytesVar:
            return 0;
        case DataType::kUnixTsNs:
            return 8;
        default:
            return 0;
    }
}

/// 定长类型在记录内的自然对齐；变长类型仅存 (off,len) 对齐到 8。
constexpr uint32_t AlignOf(DataType t) noexcept {
    switch (t) {
        case DataType::kInt8:
        case DataType::kBool:
            return 1;
        case DataType::kInt16:
            return 2;
        case DataType::kInt32:
        case DataType::kFloat32:
            return 4;
        case DataType::kInt64:
        case DataType::kFloat64:
        case DataType::kUnixTsNs:
        case DataType::kUtf8Var:
        case DataType::kBytesVar:
            return 8;
        default:
            return 1;
    }
}

inline constexpr uint32_t kMaxTableNameBytes = 64;                  ///< 含结尾 0
inline constexpr uint32_t kMaxColNameBytes = 32;                    ///< 含结尾 0
inline constexpr uint32_t kMaxInitCols = 16;                        ///< 描述符内联列数上限
inline constexpr uint32_t kMaxIndexesPerTable = 16;                 ///< 每表索引上限
inline constexpr uint64_t kTableDescMagic = 0x5254444254424C31ULL;  ///< "RTDBTBL1"
/// 建表未指定 row_cap 时的默认行槽容量。
inline constexpr uint32_t kDefaultRowCap = 1024;
/// 行槽容量上限（M1）：版本数组 = row_cap × 8B 须容身单次 slab 分配（≤64KB）。
inline constexpr uint32_t kMaxRowCap = 8192;
/// 单行定长记录字节上限（M1）：行数据区以单块 slab 承载（≤64KB）。
inline constexpr uint32_t kMaxRowFixedBytes = 32 * 1024;
/// 行数据区单块上限（M1；超限表需缩小 row_cap，段目录化列 M2）。
inline constexpr uint64_t kMaxRowDataBytes = 64 * 1024;
/// 变长池分区缺省容量（每表一块，承载该表全部 string/blob 内容）。
inline constexpr uint32_t kVarPoolRegionBytes = 64 * 1024;

/// 描述符状态字（DropTable = 墓碑态，物理数据待 compact 回收）。
enum class TableState : uint32_t { kActive = 0, kTombstone = 1 };

/// 在线加列扩展区魔数（"RTDBEXT1"）。
inline constexpr uint64_t kExtColMagic = 0x5254444245585431ULL;
/// 每表扩展列数上限（M1）：总列数 ≤ kMaxInitCols + kMaxExtCols = 32。
inline constexpr uint32_t kMaxExtCols = 16;
/// 扩展列槽字节上限（M1：定长 ≤8B 标量；宽字段/变长扩展列归 M2）。
inline constexpr uint32_t kMaxExtSlotBytes = 8;

/// 二级索引元数据魔数（"RTDBSIX1"）。
inline constexpr uint64_t kSecIdxMagic = 0x5254444253495831ULL;
/// 每表二级索引数上限（M1；与 SuperBlock.max_indexes_per_table 一致）。
inline constexpr uint32_t kMaxSecIdxPerTable = 16;

/// 共享内存二级索引元数据（docs/03 §2.2/§3.1，docs/08 §3-W5；单块 Slab 分配）。
///
/// 每个二级索引一块元数据，经 `next_off` 链成倒序创建链，表描述符
/// `reserved[0]` 指向链头（0 = 无二级索引）。B+ 树本体经
/// `tree_slot_off` 指向独立分配的 Slot 锚点。
///
/// 键布局（M1 非唯一索引的自然形态，唯一索引同布局 + 写路径前缀校验）：
/// `[EncodeOrderedInt64(值) 8B][EncodeOrderedInt64(row_no) 8B]`，
/// 同值条目按 row_no 升序连续，范围游标即可取同值多行。
struct shm_SecIdxMeta {
    uint64_t magic;         ///< 固定 kSecIdxMagic
    uint64_t next_off;      ///< 链下一个元数据偏移（0 = 尾）
    uint64_t tree_slot_off;  ///< B+ 树 Slot 锚点偏移（SharedBTree<16,uint64_t>）
    uint32_t col;           ///< 被索引列全局序号（含扩展列）
    uint32_t unique;        ///< 0/1
    uint32_t state;         ///< 预留（M1 恒 0 = 活跃）
    uint32_t reserved;
    char name[kMaxColNameBytes];  ///< 索引名（含结尾 0，UTF-8）
};
static_assert(sizeof(shm_SecIdxMeta) == 72, "shm_SecIdxMeta 必须恰好 72 字节");
static_assert(std::is_standard_layout<shm_SecIdxMeta>::value, "索引元数据必须是标准布局");

/// 共享内存在线加列扩展区（docs/03 §3.1，docs/08 §2-D5，§3-W5；单块 Slab 分配）。
///
/// 每列一个区域，经 `next_off` 链成倒序添加链（头 = 最新）；描述符
/// `ext_col_off` 指向链头，`ext_col_count` 计数（遍历以计数为准，
/// 链上多余节点为崩溃残留的孤儿区，不可达、无害）。
///
/// 区内部局（单块分配，零基址寻址）：
/// ```
/// [shm_ExtColRegion 头]
/// [slots  row_cap × slot_bytes]   ← 行槽值（未设置行为 0）
/// [set   位图 bitmap_bytes]       ← 1 = 该行显式设置过（非默认）
/// [null  位图 bitmap_bytes]       ← 1 = 显式设置且为 NULL
/// ```
/// 读取规则：未设置 → 惰性默认值（has_default 且非 NULL 时取默认标量，
/// 否则 NULL）；设置且 null 位为 1 → NULL；否则取槽值。
struct shm_ExtColRegion {
    uint64_t magic;         ///< 固定 kExtColMagic
    uint64_t next_off;      ///< 链上下一个区域偏移（0 = 尾）
    uint32_t slot_bytes;    ///< 每行槽字节数（1..kMaxExtSlotBytes）
    uint32_t bitmap_bytes;  ///< 单张位图字节数 (row_cap+7)/8
    uint32_t row_cap;       ///< 行槽容量（须等于描述符 row_cap）
    uint32_t col;           ///< 全局列序号（col_count + 添加次序）
    uint32_t layout_ver;    ///< 加入时递增后的 data_layout_ver
    uint8_t type;           ///< DataType（M1：定长 ≤8B）
    uint8_t nullable;       ///< 0/1
    uint8_t has_default;    ///< 0/1
    uint8_t default_is_null;  ///< 默认值是否为 NULL（has_default 时有效）
    uint16_t reserved;
    uint32_t reserved2;
    uint64_t default_value;  ///< 标量默认值原始 8B（整数小端/浮点 IEEE/bool 0-1）
    char name[kMaxColNameBytes];  ///< 列名（含结尾 0，UTF-8）
};
static_assert(sizeof(shm_ExtColRegion) == 88, "shm_ExtColRegion 必须恰好 88 字节");
static_assert(std::is_standard_layout<shm_ExtColRegion>::value, "扩展区头必须是标准布局");

/// 扩展区总字节数（头 + 槽区 + 两张位图）。
inline uint64_t ExtColRegionBytes(uint32_t row_cap, uint32_t slot_bytes) noexcept {
    return static_cast<uint64_t>(sizeof(shm_ExtColRegion)) +
           static_cast<uint64_t>(row_cap) * slot_bytes +
           2 * ((static_cast<uint64_t>(row_cap) + 7) / 8);
}

/// 槽区基址（区头之后）。
inline uint8_t* ExtColSlots(shm_ExtColRegion* r) noexcept {
    return reinterpret_cast<uint8_t*>(r) + sizeof(shm_ExtColRegion);
}
/// set 位图基址（槽区之后）。
inline uint8_t* ExtColSetBits(shm_ExtColRegion* r) noexcept {
    return ExtColSlots(r) + static_cast<uint64_t>(r->row_cap) * r->slot_bytes;
}
/// null 位图基址（set 位图之后）。
inline uint8_t* ExtColNullBits(shm_ExtColRegion* r) noexcept {
    return ExtColSetBits(r) + r->bitmap_bytes;
}

/// 共享内存列定义（48B 定长，标准布局）。
struct shm_ColDef {
    char name[kMaxColNameBytes];  ///< 列名（含结尾 0，UTF-8）
    uint32_t offset;              ///< 记录内字节偏移（变长列= (off,len) 槽偏移）
    uint32_t len;                 ///< 定长字节数；变长列 = 0
    uint32_t default_bits_off;    ///< 默认值位图区偏移（在线加列用；0=无）
    uint8_t type;                 ///< DataType
    uint8_t nullable;             ///< 0/1
    uint16_t flags;               ///< 预留（unique/索引提示等，M1 恒 0）
};
static_assert(sizeof(shm_ColDef) == 48, "shm_ColDef 必须恰好 48 字节");
static_assert(std::is_standard_layout<shm_ColDef>::value, "shm_ColDef 必须是标准布局");

/// 共享内存表描述符（docs/03 §3.1；1280B 定长，标准布局）。
///
/// 生命周期：CreateTable 时经目录事务写入并挂入 Catalog 双树；
/// DropTable 置 kTombstone（索引清键），物理区待 M1 compact/M2 回收。
/// 在线加列：新列定义追加进 ext 列区（ext_col_off），data_layout_ver 递增，
/// 旧 TableHandle 按其绑定的布局版本继续读取（docs/02 §3 要点 3）。
struct shm_TableDescriptor {
    // ---- 8 字节段 ----
    uint64_t magic;              ///< 固定 kTableDescMagic
    uint64_t table_id;           ///< 目录分配的单调 id（Catalog 双树键）
    uint64_t data_seg_head_off;  ///< 定长记录区头（按 rec_fixed_len × row_cap 线性寻址）
    uint64_t ver_array_off;      ///< 行版本号数组区（u64 × row_cap）
    uint64_t tombmap_off;        ///< 墓碑位图区（bit × row_cap）
    uint64_t var_pool_head;      ///< 变长池头（0 = 本表无变长列）
    uint64_t ext_col_off;        ///< 在线加列扩展区偏移（0 = 尚未加列）
    int64_t create_unix_ns;      ///< 建表时间
    // ---- 内联索引根：每表最多 kMaxIndexesPerTable 棵 B+树 ----
    /// 各索引 B+树的 Slot 锚点偏移（0 = 该槽未启用）；槽 0 固定为主键索引。
    uint64_t index_head_off[kMaxIndexesPerTable];
    // ---- 4 字节段 ----
    uint32_t rec_fixed_len;        ///< 定长记录字节数（含对齐填充）
    uint32_t row_cap;              ///< 行槽容量（创建时定格）
    uint32_t row_count;            ///< 已占用行槽数（含墓碑）
    uint32_t committed_row_count;  ///< 已提交可见行数
    uint32_t col_count;            ///< 内联列数（≤ kMaxInitCols）
    uint32_t ext_col_count;        ///< 扩展列数（在线加列追加）
    uint32_t state;                ///< TableState
    uint32_t data_layout_ver;      ///< 行布局版本（在线加列递增，旧句柄据此读）
    // ---- 字符段 ----
    char name[kMaxTableNameBytes];  ///< 表名（含结尾 0，UTF-8）
    char table_rwlock[64];          ///< 表级读写锁状态位（M1-W3 起使用，先占位清零）
    uint64_t var_pool_bytes;        ///< 变长池分区容量（0 = 本表无变长列）
    /// 预留字段语义（M1-W5-d 起启用）：
    ///  - reserved[0] = 二级索引链头偏移（`shm_SecIdxMeta` 链，0 = 无二级索引）；
    ///  - reserved[1] = 二级索引数（遍历深度；链上更深节点为崩溃残留孤儿区）。
    uint64_t reserved[19];          ///< 预留
    // ---- 内联列定义 ----
    shm_ColDef cols[kMaxInitCols];
};
static_assert(sizeof(shm_TableDescriptor) == 1280, "shm_TableDescriptor 必须恰好 1280 字节");
static_assert(std::is_standard_layout<shm_TableDescriptor>::value,
              "shm_TableDescriptor 必须是标准布局");

// ==================== 行布局计算（纯函数，W1 单测对象）====================

/// 单表物理布局的一次性计算结果。
struct RowLayoutPlan {
    Err err = Err::kOk;            ///< 非 kOk 时其余字段无意义
    uint32_t rec_fixed_len = 0;    ///< 定长记录字节数（含 null 位图与尾部对齐）
    uint32_t nullmap_offset = 0;   ///< null 位图在记录内的偏移（每列 1 bit）
    uint32_t var_col_count = 0;    ///< 变长列数
    uint64_t ver_array_bytes = 0;  ///< 行版本数组字节数
    uint64_t tombmap_bytes = 0;    ///< 墓碑位图字节数
};

constexpr uint64_t AlignUp8(uint64_t v) noexcept { return (v + 7) / 8 * 8; }

/// \brief 计算记录区/版本数组/墓碑位图尺寸与逐列偏移。
///
/// 记录布局：`[列0][列1]...[列N-1][null 位图]`，列按声明顺序自然对齐
/// （与 `AlignOf` 一致），变长列在记录内固定占用 8 字节 `(u32 off, u32 len)`
/// 槽，null 位图紧随其后（`(col_count+7)/8` 字节，位 i 对应列 i），
/// 整体对齐至 8 字节。
///
/// \param cols 列定义序列（非空）
/// \param col_count 列数
/// \param row_cap 行槽容量
/// \param out_offsets 可选输出，逐列记录内偏移（容量 ≥ col_count）
/// \return 布局计划；`err != kOk` 时其余字段无意义
inline RowLayoutPlan PlanRowLayout(const ColumnDef* cols, size_t col_count, uint32_t row_cap,
                                   uint32_t* out_offsets = nullptr) noexcept {
    RowLayoutPlan plan;
    if (cols == nullptr || col_count == 0) {
        plan.err = Err::kInvalidArgument;
        return plan;
    }
    uint32_t off = 0;
    for (size_t i = 0; i < col_count; ++i) {
        const DataType t = cols[i].type;
        const uint32_t fixed = FixedSizeOf(t);
        const uint32_t align = AlignOf(t);
        if (fixed > 0 && cols[i].len != 0 && cols[i].len < fixed) {
            plan.err = Err::kInvalidArgument;  // len 覆盖不得小于类型固有宽度
            return plan;
        }
        off = static_cast<uint32_t>((off + align - 1) / align * align);
        if (out_offsets != nullptr) out_offsets[i] = off;
        off += (fixed > 0) ? (cols[i].len != 0 ? cols[i].len : fixed) : 8;
        if (fixed == 0) plan.var_col_count++;
    }
    plan.nullmap_offset = off;
    off += static_cast<uint32_t>((col_count + 7) / 8);  // null 位图
    plan.rec_fixed_len = static_cast<uint32_t>(AlignUp8(off));
    plan.ver_array_bytes = static_cast<uint64_t>(row_cap) * 8;
    plan.tombmap_bytes = (static_cast<uint64_t>(row_cap) + 7) / 8;
    return plan;
}

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_SCHEMA_HPP_
