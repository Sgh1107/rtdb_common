# rtdb_common —— 通用实时库系统

> 基于平台现有实时库（PSRtdb）的领域经验重设计的**通用内存实时库**
> 目标：不绑定任何业务系统（SCADA/电力仅为首个参考场景），可作为独立组件嵌入任意 C++ 系统

## 一句话定位
**"共享内存零拷贝直读 + 网络访问"双模式的高性能内存表引擎，C++17 实现，单内核库 + 单服务进程交付。**

## 核心约束
| 项目 | 决策 |
|---|---|
| 语言标准 | **C++14 起、C++17 封顶**（可用 string_view/optional/variant/shared_mutex/PMR；禁用 C++20 的 span/concepts/coroutines/modules） |
| 三方依赖 | 内核**零第三方依赖**；SQL 子系统可评估引入成熟解析器；RDB 同步为插件 |
| 平台 | Windows / Linux（跨平台抽象层隔离 OS API） |
| 兼容性 | 与老 PSRtdb 不做 ABI 兼容承诺（新独立组件），但提供概念对照便于上层迁移 |

## 文档导航
1. [开发总体方案](docs/01_开发总体方案.md) —— 目标、范围、技术选型、风险
2. [系统架构设计](docs/02_系统架构设计.md) —— 分层架构、进程/部署模型、存储布局
3. [核心模块详细设计](docs/03_核心模块详细设计.md) —— 各模块类设计与关键算法
4. [API 设计规范](docs/04_API设计规范.md) —— 原生 API / C ABI / 网络协议草案
5. [交付清单与里程碑](docs/05_交付清单与里程碑计划.md) —— 全量交付物 + 分期计划 + 验收标准
6. [测试与质量保障](docs/06_测试与质量保障.md)
7. [M0 里程碑交付说明](docs/07_M0里程碑交付说明.md) —— M0 做了什么、编译运行与测试的预期结果
8. [M1 引擎开发计划](docs/08_M1引擎开发计划.md) —— M1 周计划、核心技术决策（WAL 持久化语义等）与 wal-kv 经验移植
9. [M1 里程碑交付说明](docs/09_M1交付说明.md) —— M1 交付清单、四构建矩阵、混沌×100、覆盖率与 bench 实测证据
10. [M2 接口开发计划](docs/10_M2接口开发计划.md) —— M2 周计划与 NativeApi/CAbi/RecordBlock/Result 收敛决策
11. [M2 里程碑交付说明](docs/11_M2交付说明.md) —— M2 交付清单、C ABI v1 符号集、四构建矩阵与 bench 实测
12. [代码审查报告](docs/12_代码审查报告.md) —— P0~P2 缺陷、修复冲刺记录与已知边界
13. [M3 服务开发计划](docs/13_M3服务开发计划.md) —— rtdbd/协议/客户端/订阅周计划、D1~D8 决策与落地记录
14. [M3 里程碑交付说明](docs/14_M3交付说明.md) —— M3 交付清单、出口标准对照、双机验收执行单与 CI/性能证据
15. [M3 测试执行手册](docs/15_M3测试执行手册.md) —— 本机/双机/混沌/性能测试步骤、证据模板与排障
16. [M4 完善开发计划](docs/16_M4完善开发计划.md) —— SQL 子集/Authz/PerfCounters/迁移工具/文档的 D1~D12 决策、W1~W4 周计划、M3 收口清单，以及 W1~W3 落地与验证记录（§11~§13）
17. [M4 交付说明](docs/17_M4交付说明.md) —— 交付清单、九项验收状态、已知限制与 `m4` tag 收口条件
18. [双机实测环境准备与执行手册](docs/18_双机实测环境准备与执行手册.md) —— M3 双机出口：Win 服务端 + Linux 客户端的机器/系统/工具要求、两端构建、D1~D3 命令、证据模板与排障
19. C1~C5 文档矩阵（docs/16 D10）：[C1 安装部署](docs/C1_安装部署指南.md) · [C2 API 参考](docs/C2_API参考手册.md) · [C3 布局与协议](docs/C3_布局与协议规范.md) · [C4 运维](docs/C4_运维手册.md) · [C5 老系统迁移](docs/C5_老系统迁移指南.md)

## 当前状态（M0/M1/M2/M3 已验收（`m3`）；M4 代码与文档冻结于 `0.2.0`，W4 演练/评审待留证 · 2026-11-09）

platform/infra 全部组件、SharedBTree、Slab 分配器完成并通过 **M0 验收**（验收记录见 [docs/05 §3.1](docs/05_交付清单与里程碑计划.md)，里程碑交付说明见[docs/07](docs/07_M0里程碑交付说明.md)），里程碑标记 `m0`。
**M1 引擎已验收关闭（2026-10-14，单测 113 用例）**：
表目录（双 B+ 树 + 建删表事务）、行 CRUD（主键唯一索引 + 版本乐观读 + 墓碑删除）、WAL 组提交（影像内WalRing + 覆盖协议 + 三档 fsync + `ExecuteBatch` 原子批 + 崩溃恢复重放 +failpoint 注入）、冷快照、在线加列、范围扫描、二级索引框架——出口标准两条（kill -9 混沌 ×100 verify 100/100 PASS、CRUD 全链路单测绿）+ 覆盖率门禁（95.9%/91.0%/93.9%）+ bench 五项优于基线，四构建全绿零告警，证据见[docs/09](docs/09_M1交付说明.md)。

**M2 接口已验收关闭（2026-10-17，单测 150 用例）**：
NativeApi 收敛（伞头`rtdb/rtdb.hpp`、Options 对齐 docs/04、`TableDef` 扩展、`Row`/`TableHandle::Upsert`便利面、`Result` operator* 与 RTDB_CHECK/RTDB_ASSIGN_OR_MOVE）、**RecordBlock 列式批量**（`UpsertBatch` 三阶段原子块，8 列混合稳态 1.20~1.29M 行/s ≥1M 基线）、**C ABI v1 冻结**（`rtdb_cabi.h`，14 个 `rtdb_abi_` 符号，C99 demo + Python ctypes冒烟 20 项全过）——出口标准两条（《04》示例跑通、bench 嵌入达标）+ 覆盖率门禁（97.2%/89.9%/94.8%），四构建全绿零告警，证据见 [docs/11](docs/11_M2交付说明.md)。

**M3 服务：已验收关闭（2026-11-09，tag `m3`；本机 248 用例 + 跨机 D1/D3/P99 全达标）**：
`rtdbd`（AUTH/会话 TTL/DDL/EXEC_BATCH/SCAN/SCAN_PAGE/SUBSCRIBE/FETCH_DELTA/优雅退出/握手与空闲超时/AUTH 退避）、`rtdb_client`（请求面自动重连、订阅泵有序交付/续传/溢出重取、C ABI）、网络协议 v1（20B 帧头 + CRC）与 `bench_net`/进程级混沌用例。自动化面：最新 commit `73504a7` **CI 五作业全绿**（[run 39734234285](https://github.com/Sgh1107/rtdb_common/actions/runs/39734234285)：双平台 + TSan + ASan/UBSan + 覆盖率 80% 门禁，全量 **248/248**）；P0-1~P0-5/P1-7/P1-8/P2 加固已落地（WAL 环容量可配 + 按需保留、行槽复用、二级索引重建回收、`ScanLimited` 分页 O(logN+page_rows)、C ABI v2），混沌 ×20=40/40、回环吞吐 211k 行/s、本机订阅 P99=133.7µs。**跨机双机实测已执行（2026-11-07）**：D1 订阅演示 ✅、D3 断连续传 ✅（count=1356 gaps=0 缺漏=0）、D2 跨机订阅 P99 中位 19,187 µs ✅；**D2 跨机吞吐中位 103,351 行/s**（桥接虚拟网络 + A 机 100 Mbps 链路）→ 按**方案 A** 记为已知限制：200k 明确为裸机千兆理想环境目标，性能专项归 M5，不阻塞 M3/M4；**M3 已验收关闭并打 `m3` tag**。结果见 [docs/14 §5.1](docs/14_M3交付说明.md)，M5 优化输入见 [docs/14 §5.2](docs/14_M3交付说明.md)。测试期间发现并修复了 rtdbd WAL 环尾垫片越界写崩溃（`WalRingTest.TailPad*` 回归用例，已随 `0e1d930` 提交）。Win 服务端 + Linux 客户端环境与步骤见 [docs/18 双机实测手册](docs/18_双机实测环境准备与执行手册.md)。

**M4 进展（2026-11-06）**：
W1 已落地（Authz 配置/权限门/审计、PerfCounters 基座、ADMIN(STATS)/AUTHZ_ME、`rtdbctl status`，258 用例）；W2 SQL 前端已落地（lexer/parser/AST、`kSqlSyntax=307`、`rtdbctl ddl`、fuzz 目标，283 用例，CI run `40882168913` 全绿）；**W2 四项欠账已闭环**——引擎侧 PerfCounters 全量指标与 `Engine::Metrics()` 只读快照、AUTHZ_SHOW 客户端及 `rtdbctl authz show|check`、P2-2~P2-5、`rtdbd --log-denied`；**W3 SQL 执行器已落地**（`src/sql/exec.cpp` + `Engine::ExecuteSql`：SELECT/INSERT/UPDATE/DELETE、WHERE 布尔与 NULL 语义、ORDER BY/LIMIT/OFFSET、主键下推与 `ScanLimited` 分页、`RTDB_SQL_MAX_ROWS` 预算、单语句批原子与零副作用；17 个语义用例）。`RTDB_BUILD_SQL=OFF` 时解析/执行器与 `ExecuteSql` 声明整体裁剪。**W3 剩余已闭环**（`migrate_from_psrtdb`：自描述 fixture 适配器、mapping/dry-run/校验报告/批原子迁移，7 例；`rtdbctl import|export|subscribe`；`test_compat` 4 例）。**W4 已交付**：C1~C5 文档矩阵、`scripts/chaos_m4.sh`（100 轮）/`demo_alter.sh`/`ops_drill.sh`/`smoke_subscribe.sh` 演练脚本、[docs/17 交付说明](docs/17_M4交付说明.md)、版本串 `0.2.0`（C ABI v2 / shm 布局 v2 不变）；CI run `42594636299`（`1390bcb`）五作业全绿。九项验收 **7/9 闭环**（混沌 100 轮 200/200、在线加列 5/5、C4 故障演练、`subscribe` 冒烟均已实操留证，见 [演练记录](docs/evidence/M4_演练记录.md)）；默认端口与写者心跳口径已于 2026-11-10 统一。**尚未收口**：真实 PSRtdb 老映像比对（格式不可得，建议记为已知限制）、C1~C5 文档评审、收口提交的 CI 复跑——故 `m4` tag 暂不打（逐项状态与收口条件见 docs/17 §4/§7）。决策 D1~D12 与周计划见 [docs/16](docs/16_M4完善开发计划.md)，W1~W3 落地与验证记录见 §11~§13。

| 组件 | 位置 | 状态 |
|---|---|---|
| 公共头（version/result/err/options） | `include/rtdb/` | ✅ |
| SharedMemoryFile 平台抽象 | `src/platform/platform.hpp` | ✅ Win32/Linux 双实现 |
| SuperBlock + 布局 CRC 自校验 | `src/infra/layout.hpp`、`shm_region.*` | ✅ |
| Slab 共享内存分配器（32B~64KB 12 级，精确计账） | `src/infra/slab_allocator.*` | ✅ |
| 根互斥锁（跨进程互斥 + 死亡恢复） | `src/infra/shm_mutex.*` | ✅ |
| 共享内存 B+ 树（插入/点查/游标/惰性删除/分裂） | `src/infra/shared_btree.hpp` | ✅ |
| Engine 门面（打开/校验/全局状态字） | `include/rtdb/engine.hpp` | ✅ 最小可用 |
| rtdbd 服务端（RPC/会话/订阅泵/优雅退出） | `src/server/` | ✅ M3 自动化验收 |
| rtdb_client（C++/C 双面 + 订阅泵 + 自动重连） | `src/client/`、`include/rtdb/rtdb_client*` | ✅ M3 自动化验收 |
| 内嵌 SQL 单表子集（解析 + 执行，`RTDB_BUILD_SQL` 可裁剪） | `include/rtdb/sql.hpp`、`src/sql/` | ✅ M4-W2/W3 |
| Authz（表级权限/审计）与 PerfCounters（引擎指标快照） | `src/server/authz.*`、`perf.hpp`、`Engine::Metrics()` | ✅ M4-W1/W2 |
| rtdbctl 运维 CLI（status/ddl/authz） | `tools/rtdbctl` | ✅ M4-W2 |
| 单元测试（299 例 + BenchNetCi，含进程级混沌/恢复注入/基准门禁） | `tests/unit/`、`tools/bench_net` | ✅ |
| CI 五 job 全绿（双平台构建+单测 / ASan+UBSan / TSan / 覆盖率 80% 门禁） | `.github/workflows/ci.yml` | ✅ |
| clang-format / clang-tidy | `.clang-format`、`.clang-tidy` | ✅（全仓 4 空格风格统一，暂不进 CI 强制） |

**快速构建（Windows）**
```powershell
cmake -S . -B build -A x64   # 不固定生成器版本，自动探测本机最新 VS（兼容 VS 2022/2026）
cmake --build build --config Release
cd build; ctest -C Release --output-on-failure   # M0 冒烟测试
```
注意：MSVC 需 `/utf-8` 编译选项（CMakeLists 已内置）；ctest 请从 `build/` 目录执行。

## 设计源头
老实时库分析见 [`db/rtdb/docs/RTDB实时库系统设计总结.md`](../db/rtdb/docs/RTDB实时库系统设计总结.md)。继承其架构决策（共享内存直读、自描述元数据、声明式同步配置），替换其实现与平台耦合。
