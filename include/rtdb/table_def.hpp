#ifndef RTDB_TABLE_DEF_HPP_
#define RTDB_TABLE_DEF_HPP_

/// \file
/// \brief 表定义（schema）公共类型：列类型系统与建表入参。
///
/// 本头文件是宿主侧类型，允许使用标准库容器与字符串；
/// 共享内存内对应的定长结构见 `src/storage/schema.hpp`（`shm_ColDef` /
/// `shm_TableDescriptor`），二者字段语义保持一致（docs/03 §3.1）。

#include <cstdint>
#include <string>
#include <vector>

namespace rtdb {

/// \brief 列数据类型。
///
/// 枚举值随 schema 持久化到共享内存，**只增不改**：新增类型只能追加到尾部，
/// 已有取值不得复用或调整，以保证存量映像可读（docs/08 §3-W1）。
enum class DataType : uint8_t {
    kInt8 = 1,       ///< 8 位有符号整数
    kInt16 = 2,      ///< 16 位有符号整数
    kInt32 = 3,      ///< 32 位有符号整数
    kInt64 = 4,      ///< 64 位有符号整数
    kFloat32 = 5,    ///< 32 位浮点数
    kFloat64 = 6,    ///< 64 位浮点数（索引键需可排序序数变换，docs/03 §2.2）
    kBool = 7,       ///< 布尔值（1 字节，0/1）
    kUtf8Var = 8,    ///< 变长 UTF-8 字符串，内容在变长池，记录内为 (off,len)
    kBytesVar = 9,   ///< 变长二进制块，其余同 kUtf8Var
    kUnixTsNs = 10,  ///< 语义为 Unix 纳秒时间戳的定长 64 位整数
};

/// \brief 列默认值（M1：定长 ≤8B 标量；变长列默认值归 M2）。
///
/// 用于在线加列（`Engine::AlterTableAddColumn`）时对存量行的填充：
/// 未显式设置的行按默认值读取（惰性，不物化）。
struct ColumnDefault {
    /// 默认 NULL（is_null=true 时其余字段无意义）。
    bool is_null = true;
    /// 整数类/布尔/UnixTsNs 标量值。
    int64_t int_value = 0;
    /// 浮点类标量值。
    double real_value = 0.0;
};

/// \brief 宿主侧列定义（`Engine::CreateTable` / `AlterTableAddColumn` 入参元素）。
struct ColumnDef {
    /// 列名（UTF-8，不含结尾 0，长度限制见 `kMaxColNameBytes - 1`）。
    std::string name;

    /// 列数据类型。
    DataType type = DataType::kInt64;

    /// 定长列的字节数覆盖；0 表示按类型固有宽度（变长列恒为 0）。
    /// 非 0 时不得小于类型固有宽度。
    uint32_t len = 0;

    /// 是否允许空值。
    bool nullable = true;

    /// 是否携带默认值（在线加列时用于旧行填充，M1-W5 启用）。
    /// 建表时忽略（无存量行）；加列时 has_default=false 仅 nullable 列允许。
    bool has_default = false;

    /// 默认值（has_default 为 true 时有效）。
    ColumnDefault default_value;
};

/// \brief 宿主侧建表定义（docs/03 §4.2 `TableDef` 的 M1 子集）。
struct TableDef {
    /// 表名（UTF-8，非空，长度限制见 `kMaxTableNameBytes - 1`）。
    std::string name;

    /// 行槽容量；0 表示由引擎取默认值（`kDefaultRowCap`）。
    uint32_t row_cap = 0;

    /// 列定义序列（至少 1 列，不超过 `kMaxInitCols`）。
    std::vector<ColumnDef> columns;
};

/// \brief 二级索引定义（`Engine::CreateSecondaryIndex` 入参；M1-W5 框架）。
///
/// M1 边界：被索引列须为可有序编码的定长标量（整数类/浮点类/布尔/
/// UnixTsNs；变长列归 M2）；键布局为 `[有序值][有序 row_no]`（非唯一
/// 索引的自然形态，同值多行按 row_no 升序连续）；唯一性在写路径
/// （暂存阶段）强制，建索引时对存量数据校验。
struct SecondaryIndexDef {
    /// 索引名（表内唯一，非空，长度限制见 `kMaxColNameBytes - 1`）。
    std::string name;

    /// 被索引列序号（0 起；含在线扩展列，即 ≥ `column_count` 时指向扩展列）。
    uint32_t column_index = 0;

    /// 是否唯一索引（true 时同值只允许一行；NULL 不入索引、不参与唯一性）。
    bool unique = false;
};

}  // namespace rtdb

#endif  // RTDB_TABLE_DEF_HPP_
