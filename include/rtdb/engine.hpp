#ifndef RTDB_ENGINE_HPP_
#define RTDB_ENGINE_HPP_

#include <cstdint>
#include <memory>

#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"

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
/// M0 阶段职责（当前）：
///   - 打开/创建文件映像共享内存区，校验 SuperBlock；
///   - 维护 committed_seq / session_count 等全局状态字。
/// M1 起追加：CreateTable/OpenTable/CRUD/Batch/Scan/Subscribe —— 见
/// docs/03 §4.2 的目标签名。
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
  std::unique_ptr<Impl> impl_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace rtdb

#endif  // RTDB_ENGINE_HPP_
