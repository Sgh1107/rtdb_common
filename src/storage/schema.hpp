#ifndef RTDB_SRC_STORAGE_SCHEMA_HPP_
#define RTDB_SRC_STORAGE_SCHEMA_HPP_

/// \file 表 schema 类型系统与行布局计算（docs/03 §3.1，docs/08 §3-W1）。
///
/// 约定：
///  - 本头文件全部为 shm_ 前缀的标准布局结构（驻留映像）与宿主侧定义类型
///    的边界，不含任何成员函数/虚函数/裸指针/std 容器；
///  - 映像内结构体靠 pointer_width 校验与同机字长保障可读性，不做字节序
///    序列化（WAL 记录才做逐字段小端编码，见 docs/08 §2-D3）；
///  - 布局计算函数全部为 constexpr/纯函数，单测可直接覆盖。

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "rtdb/err.hpp"

namespace rtdb {
namespace storage {

/// 列数据类型。编码值随 schema 持久化，**只增不改**（新增追加到尾部）。
enum class DataType : uint8_t {
    kInt8 = 1,
    kInt16 = 2,
    kInt32 = 3,
    kInt64 = 4,
    kFloat32 = 5,
    kFloat64 = 6,  ///< 索引键经可排序序数变换（docs/03 §2.2）
    kBool = 7,
    kUtf8Var = 8,    ///< 变长字符串：记录内存 (pool_off, len)
    kBytesVar = 9,   ///< 变长二进制：同上
    kUnixTsNs = 10,  ///< 语义为 Unix 纳秒时间戳的定长 i64
};

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

/// 描述符状态字（DropTable = 墓碑态，物理数据待 compact 回收）。
enum class TableState : uint32_t { kActive = 0, kTombstone = 1 };

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
    char table_rwlock[64];          ///< 表级读写锁状态位（M1-W2 接管，先占位清零）
    char reserved[160];
    // ---- 内联列定义 ----
    shm_ColDef cols[kMaxInitCols];
};
static_assert(sizeof(shm_TableDescriptor) == 1280, "shm_TableDescriptor 必须恰好 1280 字节");
static_assert(std::is_standard_layout<shm_TableDescriptor>::value,
              "shm_TableDescriptor 必须是标准布局");

// ==================== 宿主侧定义类型（可含 std 类型）====================

/// 建表时的列定义（宿主侧，Open/CreateTable 入参）。
struct ColDefHost {
    std::string name;
    DataType type;
    uint32_t len = 0;  ///< 定长列的字节数覆盖（0 = 按 FixedSizeOf）；变长列恒 0
    bool nullable = true;
    bool has_default = false;
};

/// 建表定义（docs/03 §4.2 TableDef 的 M1 子集）。
struct TableDefHost {
    std::string name;
    uint32_t row_cap = 0;  ///< 0 = 取引擎默认（按映像剩余容量在 W3 定）
    std::vector<ColDefHost> cols;
};

// ==================== 行布局计算（纯函数，W1 单测对象）====================

/// 单表物理布局的一次性计算结果。
struct RowLayoutPlan {
    Err err = Err::kOk;            ///< 非 kOk 时其余字段无意义
    uint32_t rec_fixed_len = 0;    ///< 定长记录字节数（含尾部对齐填充至 8）
    uint32_t var_col_count = 0;    ///< 变长列数
    uint64_t ver_array_bytes = 0;  ///< 行版本数组字节数
    uint64_t tombmap_bytes = 0;    ///< 墓碑位图字节数
};

constexpr uint64_t AlignUp8(uint64_t v) noexcept { return (v + 7) / 8 * 8; }

/// 依据列定义序列计算记录区/版本数组/墓碑位图尺寸。
/// 列偏移按声明顺序做自然对齐排布（与 AlignOf 一致）；变长列在记录内
/// 固定占用 8 字节 (u32 off + u32 len) 槽。
/// \param out_offsets 可选输出：逐列的记录内偏移（容量 ≥ col_count）。
RowLayoutPlan PlanRowLayout(const ColDefHost* cols, size_t col_count, uint32_t row_cap,
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
    plan.rec_fixed_len = static_cast<uint32_t>(AlignUp8(off));
    plan.ver_array_bytes = static_cast<uint64_t>(row_cap) * 8;
    plan.tombmap_bytes = (static_cast<uint64_t>(row_cap) + 7) / 8;
    return plan;
}

}  // namespace storage
}  // namespace rtdb

#endif  // RTDB_SRC_STORAGE_SCHEMA_HPP_
