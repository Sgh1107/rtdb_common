#ifndef RTDB_ENGINE_HPP_
#define RTDB_ENGINE_HPP_

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "rtdb/table_handle.hpp"

namespace rtdb {

namespace infra {
class SlabShmAllocator;
class ShmRootMutex;
}  // namespace infra

#if defined(_MSC_VER)
#pragma warning(push)
// C4251: PImpl 成员不需要 DLL 接口 —— 所有成员函数均在 DLL 内实现，
// 客户端不会触碰 Impl 的实例化，属预期内安全用法。
#pragma warning(disable : 4251)
#endif

/// 实时库引擎门面（facade）。
///
/// 各阶段职责：
///   - M0：打开/创建文件映像共享内存区，校验 SuperBlock，维护
///     committed_seq / session_count 等全局状态字；
///   - M1-W2（当前）：表目录 —— `CreateTable` / `OpenTable` / `DropTable`，
///     目录持久化于映像内双 B+ 树；
///   - M1-W3 起追加：CRUD / 批量 / 扫描（见 docs/03 §4.2 目标签名）。
///
/// 线程安全：同一实例可被多线程使用；写操作内部经根互斥锁串行化。
/// 不同进程通过同一映像文件挂载共享数据（docs/02 §2）。
class RTDB_API Engine {
public:
    Engine() noexcept;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    /// 打开或创建实例。语义：
    ///  - 文件不存在 → 按 Options 创建映像并初始化 SuperBlock；
    ///  - 已存在     → 校验 magic/格式版本/机器字宽/layout_crc，
    ///                 任一不匹配返回对应错误码（kBadSuperBlock /
    ///                 kVersionMismatch / kIncompatibleLayout）；
    ///  - Reader 角色以只读映射打开；其他角色读写映射。
    static Result<std::unique_ptr<Engine>> Open(const Options& opts);

    /// 全局提交序列号：所有已持久化变更的最大单调序号。
    /// 订阅游标机制（M3）以此为基准。当前阶段尚无事务提交点，
    /// 恒等于初始化值。
    int64_t CommittedSeq() const noexcept;

    /// 本映像被打开过的累计次数（每次成功 Open 递增一次）。
    /// 用作跨进程活性/持久化的最小可见证据。
    int64_t SessionCount() const noexcept;

    // ------------------ 表目录（M1-W2）------------------
    /// \brief 建立新表。
    ///
    /// 校验表名/列定义/上限，分配并填充描述符后挂入目录双 B+ 树；
    /// 任一步失败完整回滚。
    ///
    /// \param def 建表定义（表名非空、至少 1 列）
    /// \return 表句柄；失败码见 `Err`：
    ///         `kInvalidArgument` / `kLimitExceeded` / `kTableExists` /
    ///         `kReadOnlyHandle` / `kUnsupported` / `kOutOfMemory`
    Result<TableHandle> CreateTable(const TableDef& def);

    /// \brief 按名打开已存在的表。
    /// \param name 表名
    /// \return 表句柄；不存在或已删除返回 `kTableNotFound`
    Result<TableHandle> OpenTable(std::string_view name);

    /// \brief 删除表（置描述符墓碑态并摘除表名索引）。
    ///
    /// 物理数据区不随删表释放，待后续 compact 回收；表名可被新表复用。
    ///
    /// \param name 表名
    /// \return kOk；不存在返回 `kTableNotFound`；只读角色返回 `kReadOnlyHandle`
    Result<void> DropTable(std::string_view name);

    // ------------------ 行 CRUD（M1-W3）------------------
    /// \brief 插入一行。列 0 为主键（整数类、非空），主键冲突返回
    /// `kAlreadyExists`；行槽耗尽返回 `kOutOfMemory`。
    /// \param table 目标表句柄
    /// \param row 列值序列（长度须等于表列数，顺序与建表列序一致）
    /// \return 分配到的行槽号
    Result<uint64_t> Insert(const TableHandle& table, const std::vector<ColumnValue>& row);

    /// \brief 按主键读取整行（免锁乐观读，返回自持有值）。
    /// \return 列值序列；行不存在或已删除返回 `kNotFound`
    Result<std::vector<ColumnValue>> GetByKey(const TableHandle& table, const ColumnValue& key);

    /// \brief 按主键整行覆盖更新（主键不可变更；值须置于 row[0]）。
    /// \return kOk / kNotFound（行不存在）/ kInvalidArgument / kReadOnlyHandle
    Result<void> UpdateByKey(const TableHandle& table, const ColumnValue& key,
                             const std::vector<ColumnValue>& row);

    /// \brief 按主键删除行（置墓碑并摘除索引键；行槽待 compact 回收）。
    /// \return kOk；行不存在返回 `kNotFound`
    Result<void> DeleteByKey(const TableHandle& table, const ColumnValue& key);

    /// \brief 按主键判定行是否存在。
    Result<bool> ContainsKey(const TableHandle& table, const ColumnValue& key);

    // ------------------ 写侧全局锁与内存管理（M0 直通句柄）------------------
    /// 获取根互斥锁（保护分配器 freelist 与未来目录树的全局变更；
    /// 可恢复持有者崩溃后的残留死锁）。timeout_ms==0 表示纯尝试。
    Err LockRoot(uint32_t timeout_ms) noexcept;
    void UnlockRoot() noexcept;

    /// 共享内存 slab 分配器句柄。调用方负责持锁；Reader 恒为 nullptr。
    infra::SlabShmAllocator* RawAllocator() const noexcept;
    /// 根互斥锁对象本体（供高级用法），Reader 恒为 nullptr。
    infra::ShmRootMutex* RawRootMutex() const noexcept;

private:
    struct Impl;

    /// \brief 由描述符字段装配表句柄（内部；仅传递原始字段，不泄漏内部类型）。
    static TableHandle MakeHandle(int64_t table_id, const char* name, uint32_t layout_version,
                                  uint32_t col_count, uint32_t row_cap, uint64_t desc_off);

    std::unique_ptr<Impl> impl_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_ENGINE_HPP_
