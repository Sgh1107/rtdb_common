#ifndef RTDB_SRC_INFRA_FAILPOINT_HPP_
#define RTDB_SRC_INFRA_FAILPOINT_HPP_

/// \file failpoint 故障注入点（docs/06 §3 混沌注入矩阵的进程内实现，docs/08 §3-W4）。
///
/// 两种注入模式（均为**一次性**，命中后自动解除）：
///  - kCrash：到达注入点立即硬退出（quick_exit，不执行任何析构/解锁），
///    等价 kill -9。供跨进程崩溃注入用例：父进程武装注入点并拉起子进程，
///    子进程在指定注入点死亡，父进程重开映像验证恢复语义。
///  - kError：注入点返回 true，调用方将本次操作按真实失败路径处理
///    （返回对应错误码），进程存活。供进程内错误路径用例。
///
/// 开销：全局开关为原子布尔，未武装任何注入点时命中检查只有一次
/// relaxed load（热路径近零成本）。生产路径不调用武装 API 即无注入。
///
/// 环境变量（子进程注入用，父进程 spawn 前设置、spawn 后清除）：
///   RTDB_FAILPOINTS="wal.insert.before=crash,wal.batch.mid=error"
/// 首次使用任一 API 时惰性解析一次（线程安全）；此后修改环境变量不再生效，
/// 程序化武装（`Arm`）始终可用。
///
/// 实现说明（W4 本机验证修复）：注入状态（武装表 + 一次性开关）必须跨
/// DLL/EXE 边界唯一——Windows 下内联函数的函数局部静态在模块间**不共享**，
/// 若状态留在头文件里，测试进程 `Arm` 的注入点对 rtdbcore.dll 内的
/// CrashPoint/ErrorPoint 不可见（Linux 因 COMDAT 折合侥幸一致，属潜伏
/// 不一致）。故全部状态与逻辑收敛于 src/infra/failpoint.cpp（随 rtdbcore
/// 编译），本头仅保留声明；调测双方经导入/导出符号访问同一份状态。
///
/// 注入点清单（W4，docs/06 §3 五点 + 错误注入）：
///   wal.insert.before   崩溃  Insert 校验后、WAL 追加前
///   wal.append.mid      崩溃  WAL 记录头部已写、载荷+CRC 未写（撕裂记录）
///   wal.apply.mid       崩溃  行数据已发布、主键索引未发布
///   wal.commit.before   崩溃  变更已应用、committed_seq 未推进
///   wal.batch.wal_mid   崩溃  批 WAL 阶段（BEGIN 与 COMMIT 之间）
///   wal.batch.mid       崩溃  批应用阶段（部分操作已应用）
///   wal.append.fail     错误  模拟 WAL 追加失败（调用方按真实失败语义毒化）
///   wal.index.insert    错误  模拟主键索引插入失败（应用阶段失败）
/// 注入点清单（W5 追加）：
///   snapshot.before_copy 崩溃 冷快照拷贝前（checkpoint/环回收已落、
///                              副本缺失或部分——活映像须不受影响）
///   alter.before_link    崩溃 加列区域已写、尚未挂链（表 schema 须不变）
///   secidx.before_link   崩溃 二级索引元数据已写、尚未挂链（表无此索引，
///                              存量数据与可写性须不受影响）

#include <cstdint>

#include "rtdb/api_macro.hpp"

namespace rtdb {
namespace failpoint {

enum class Mode : uint8_t { kCrash = 0, kError = 1 };

/// 崩溃注入的退出码（与 test_main 子进程死亡演示一致，父用例据此判别）。
inline constexpr int kCrashExitCode = 9;

/// \brief 程序化武装注入点（测试用）。
RTDB_API void Arm(const char* name, Mode mode);

/// \brief 解除全部注入点（测试 TearDown 用）。
RTDB_API void ClearAll();

/// \brief 重新解析 RTDB_FAILPOINTS 并武装（先清空既有注入点）。
///
/// 供崩溃注入子进程在完成前置阶段（如建表 DDL）后重置注入时点——
/// 子进程先 ClearAll 解除（避免注入点命中于前置阶段），完成前置阶段
/// 后调用本函数恢复规格指定的注入，再执行目标负载。
RTDB_API void ResetFromEnv();

/// \brief 崩溃型注入点：若已武装则硬退出进程（不返回）。
RTDB_API void CrashPoint(const char* name);

/// \brief 错误型注入点：命中返回 true（一次性，调用方按失败路径处理）。
RTDB_API bool ErrorPoint(const char* name);

}  // namespace failpoint
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_FAILPOINT_HPP_
