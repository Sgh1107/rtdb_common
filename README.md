# rtdb_common —— 通用实时库系统

> 基于平台现有实时库（PSRtdb）的领域经验重设计的**通用内存实时库**。
> 目标：不绑定任何业务系统（SCADA/电力仅为首个参考场景），可作为独立组件嵌入任意 C++ 系统。

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

## 当前状态（M0 已验收 · M1-W1 进行中 · 2026-08）

platform/infra 全部组件、SharedBTree、Slab 分配器完成并通过 **M0 验收**
（验收记录见 [docs/05 §3.1](docs/05_交付清单与里程碑计划.md)，里程碑交付说明见
[docs/07](docs/07_M0里程碑交付说明.md)），里程碑标记 `m0`。
**M1 引擎已开工**：D1 拍板固化（映像内 WalRing + 三档持久化语义）、SuperBlock v2、
Err 3xx 段、storage schema 类型系统与行布局计算、MVarLenPool 变长池均已落地
（单测 41 用例全绿），计划见 [docs/08](docs/08_M1引擎开发计划.md)。

| 组件 | 位置 | 状态 |
|---|---|---|
| 公共头（version/result/err/options） | `include/rtdb/` | ✅ |
| SharedMemoryFile 平台抽象 | `src/platform/platform.hpp` | ✅ Win32/Linux 双实现 |
| SuperBlock + 布局 CRC 自校验 | `src/infra/layout.hpp`、`shm_region.*` | ✅ |
| Slab 共享内存分配器（32B~64KB 12 级，精确计账） | `src/infra/slab_allocator.*` | ✅ |
| 根互斥锁（跨进程互斥 + 死亡恢复） | `src/infra/shm_mutex.*` | ✅ |
| 共享内存 B+ 树（插入/点查/游标/惰性删除/分裂） | `src/infra/shared_btree.hpp` | ✅ |
| Engine 门面（打开/校验/全局状态字） | `include/rtdb/engine.hpp` | ✅ 最小可用 |
| 单元测试（25 用例，含两进程死亡恢复演示） | `tests/unit/` | ✅ |
| CI 四 job 全绿（双平台构建+单测 / ASan+UBSan / 覆盖率 86%） | `.github/workflows/ci.yml` | ✅ |
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
