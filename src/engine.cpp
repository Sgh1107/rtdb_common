#include "rtdb/engine.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "infra/failpoint.hpp"
#include "infra/layout.hpp"
#include "infra/shm_mutex.hpp"
#include "infra/shm_region.hpp"
#include "platform/ipc_wait.hpp"
#include "storage/alter.hpp"
#include "storage/catalog.hpp"
#include "storage/group_commit.hpp"
#include "storage/sec_index.hpp"
#include "storage/snapshot.hpp"
#include "storage/table_store.hpp"
#include "storage/wal_ring.hpp"

namespace rtdb {

namespace {

/// 引擎内部临界区加锁超时（毫秒）：根锁持有者崩溃时由死亡恢复在
/// 一次活性探测内完成，正常路径远快于此值。
inline constexpr uint32_t kEngineLockTimeoutMs = 5000;

/// 表列元数据（init 列 + ext 列统一视图；M2 Upsert/UpsertBatch 共用）。
struct ColInfo {
    const char* name = nullptr;
    bool nullable = false;
    bool has_default = false;
    bool default_is_null = false;
    uint64_t default_raw = 0;
    DataType type = DataType::kInt64;
};

/// 收集 [0, visible) 列元数据：init 列取描述符、ext 列走区域链。
/// 链缺环（meta 未覆盖）返回 kCorruption。
Err CollectColumnInfo(void* base, const storage::shm_TableDescriptor* desc, uint32_t visible,
                      std::vector<ColInfo>* out) {
    if (desc == nullptr || out == nullptr) return Err::kInvalidArgument;
    out->assign(visible, ColInfo{});
    for (uint32_t i = 0; i < visible && i < desc->col_count; ++i) {
        (*out)[i].name = desc->cols[i].name;
        (*out)[i].nullable = desc->cols[i].nullable != 0;
        (*out)[i].type = static_cast<DataType>(desc->cols[i].type);
    }
    if (visible > desc->col_count) {
        std::vector<storage::shm_ExtColRegion*> regions;
        if (!IsOk(storage::WalkExtColumns(base, desc, &regions))) return Err::kCorruption;
        for (storage::shm_ExtColRegion* r : regions) {
            if (r->col >= desc->col_count && r->col < visible) {
                ColInfo& ci = (*out)[r->col];
                ci.name = r->name;
                ci.nullable = r->nullable != 0;
                ci.has_default = r->has_default != 0;
                ci.default_is_null = r->default_is_null != 0;
                ci.default_raw = r->default_value;
                ci.type = static_cast<DataType>(r->type);
            }
        }
        for (uint32_t i = desc->col_count; i < visible; ++i) {
            if ((*out)[i].name == nullptr) return Err::kCorruption;
        }
    }
    return Err::kOk;
}

/// ext 列默认值逆解码（EncodeExtDefault 的逆，见 storage/alter.cpp）。
ColumnValue DecodeExtDefault(const ColInfo& ci) {
    if (ci.default_is_null) return ColumnValue::Null();
    if (ci.type == DataType::kFloat32) {
        float f = 0.0F;
        std::memcpy(&f, &ci.default_raw, sizeof(f));
        return ColumnValue::Real(static_cast<double>(f));
    }
    if (ci.type == DataType::kFloat64) {
        double d = 0.0;
        std::memcpy(&d, &ci.default_raw, sizeof(d));
        return ColumnValue::Real(d);
    }
    return ColumnValue::Int(static_cast<int64_t>(ci.default_raw));
}

/// 列式载荷类型与 schema 列类型兼容性（M2-W2）：int64 载荷供整数类/
/// bool/时间戳列，double 供浮点列，string_view 供变长列。
bool PayloadTypeCompatible(DataType payload, DataType column) {
    switch (payload) {
        case DataType::kInt64:
            return column == DataType::kInt8 || column == DataType::kInt16 ||
                   column == DataType::kInt32 || column == DataType::kInt64 ||
                   column == DataType::kBool || column == DataType::kUnixTsNs;
        case DataType::kFloat64:
            return column == DataType::kFloat32 || column == DataType::kFloat64;
        case DataType::kUtf8Var:
            return column == DataType::kUtf8Var || column == DataType::kBytesVar;
        default:
            return false;
    }
}

/// 单条行操作的 WAL 记录字节数（A1 批前预留用；与 WalAppendRowOp 的
/// 载荷选取规则同构：insert/update 载荷 = 整行记录，delete = 主键 8B）。
uint64_t StagedWalBytes(const storage::StagedRowOp& st) noexcept {
    switch (st.kind) {
        case storage::StagedRowOp::Kind::kInsert:
        case storage::StagedRowOp::Kind::kUpdate:
            return storage::WalRing::RecordBytes(static_cast<uint32_t>(st.rec.size()));
        case storage::StagedRowOp::Kind::kDelete:
            return storage::WalRing::RecordBytes(8);
        default:
            return 0;
    }
}

}  // namespace

struct Engine::Impl {
    /// 映像文件路径（冷快照拷贝的派生基准；由 Options.path 带入）。
    std::string image_path;
    std::unique_ptr<ShmRegion> region;
    std::unique_ptr<storage::Catalog> catalog;
    /// 表行存储句柄缓存（键 = 描述符偏移）；首次访问时按需附着。
    /// 并发约定（P1-2 修复）：读路径（GetByKey/Scan/Upsert 校验等）在根锁
    /// 外调用 StoreFor，故容器本身由 stores_mu 保护（首次附着的 emplace
    /// 与并发读的 find 可发生于不同线程）。
    std::unordered_map<uint64_t, std::unique_ptr<storage::TableStore>> stores;
    std::mutex stores_mu;
    /// 映像内 WAL 环（写侧角色挂载；Reader 恒为空）。
    std::unique_ptr<storage::WalRing> ring;
    /// 组提交协调器（写侧角色挂载）。
    std::unique_ptr<storage::GroupCommit> commit;
    /// 批 WAL 进行中标志（A1 纵深防御）：批内 EnsureRingSpace 绝不允许
    /// 丢弃保留记录（BEGIN 之后的前缀未提交，丢弃即破坏批原子性）。
    /// 由 ReserveBatchWal 置位、批 WAL 段结束时复位；仅在根锁内读写。
    bool batch_in_progress = false;

    // ---- M2-W2 列式批复用缓冲（UpsertBatch 专用）----
    // 根锁内串行使用（并发 UpsertBatch 由根锁串行化），容量随高水位
    // 保留：跨批次复用 StagedRowOp 的 rec/old_rec 缓冲与行值 vector，
    // 消除每批 ~N 次小分配（百万行/s 的实测杠杆之一，docs/10 §2-M2-D2）。
    std::vector<storage::StagedRowOp> block_staged;
    std::vector<ColumnValue> block_vals;

    Impl();
    ~Impl();  // 停后台线程 + 优雅收尾全映像 flush

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// \brief 取（必要时附着）表行存储，并校验句柄与描述符自洽。
    Result<storage::TableStore*> StoreFor(uint64_t desc_off, int64_t expected_table_id) {
        storage::TableStore* store = nullptr;
        {
            // 仅保护容器本身（find/emplace）；attach 与后续校验不依赖容器锁。
            std::lock_guard<std::mutex> lk(stores_mu);
            auto it = stores.find(desc_off);
            if (it == stores.end()) {
                auto attach = storage::TableStore::Attach(region->base_address(),
                                                          region->allocator(), desc_off);
                if (!attach.IsOk()) return Result<storage::TableStore*>::Fail(attach.Error());
                it = stores.emplace(desc_off, attach.TakeValue()).first;
            }
            store = it->second.get();
        }
        const storage::shm_TableDescriptor* desc = store->descriptor();
        if (desc == nullptr) return Result<storage::TableStore*>::Fail(Err::kCorruption);
        if (desc->table_id != static_cast<uint64_t>(expected_table_id))
            return Result<storage::TableStore*>::Fail(Err::kInvalidArgument);
        if (desc->state != static_cast<uint32_t>(storage::TableState::kActive))
            return Result<storage::TableStore*>::Fail(Err::kTableNotFound);
        return Result<storage::TableStore*>::Ok(store);
    }

    /// \brief 提交标记：推进全局 committed_seq（写侧串行，单调）。
    /// 同时唤醒全部订阅等待者（futex/WaitOnAddress 等待其低 32 位字；
    /// 无等待者时无副作用，docs/13 §3-D4）。
    void MarkCommitted(uint64_t seq) noexcept {
        auto* sb = region->super_block();
        layout::AsAtomicU64(&sb->committed_seq)->store(seq);
        platform::WakeAll32(reinterpret_cast<uint32_t*>(&sb->committed_seq));
    }

    /// \brief 行宽校验（W5-b）：insert/update 的列值数须等于句柄可见
    /// 列数（建表列 + 句柄绑定时刻的扩展列）。旧句柄按旧 schema 读写，
    /// 其发放后新增的列对旧句柄不可见（docs/02 §3 要点 3）。
    static Err CheckRowWidth(const TableHandle& table, size_t row_size) noexcept {
        return row_size == table.visible_column_count() ? Err::kOk : Err::kInvalidArgument;
    }

    /// \brief 毒化引擎（WAL 写失败/应用失败后拒写保读，docs/08 §2-D2）。
    void Poison() noexcept {
        if (ring != nullptr) ring->MarkPoisoned();
    }

    /// \brief 二级索引延迟重建（M1-W5-d）：重放收敛后，对含二级索引的
    /// 活跃表全量重建（WAL 不记索引变更，Open 自愈，docs/08 §3-W5）。
    /// 墓碑表物理区待 compact 回收，其索引链随之废弃，不参与重建。
    Err RebuildSecondaryIndexes() noexcept {
        if (catalog == nullptr || region->allocator() == nullptr) return Err::kUnsupported;
        infra::ShmRootMutexGuard guard(region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return guard.error();
        Err last = Err::kOk;
        catalog->ForEachTable([this, &last](uint64_t desc_off,
                                            const storage::shm_TableDescriptor* desc) {
            if (!IsOk(last)) return;  // 首个失败即止（保持错误可归因）
            if (desc->state != static_cast<uint32_t>(storage::TableState::kActive)) return;
            if (storage::SecIdxHeadOff(desc) == 0) return;  // 无二级索引：零开销
            auto store_res = StoreFor(desc_off, static_cast<int64_t>(desc->table_id));
            if (!store_res.IsOk()) {
                last = store_res.Error();
                return;
            }
            const Err e = store_res.Value()->RebuildSecondaryIndexes();
            if (!IsOk(e)) last = e;
        });
        return last;
    }

    /// \brief 覆盖协议（docs/08 §2-D1"覆盖前先落盘"）：环空间不足时强制
    /// 全映像 flush、推进 checkpoint_seq、丢弃全部保留记录。
    ///
    /// A1 纵深防御：批 WAL 进行中拒绝丢弃（BEGIN 之后的前缀未提交，丢弃即
    /// 静默破坏批原子性）。正常路径由批前预留（ReserveBatchWal）保证批内
    /// 不触发本分支；触发即为缺陷兜底——返回 kLimitExceeded，批调用方
    /// 毒化引擎，宁可失败不丢数据。
    ///
    /// \return kOk / kIoError（flush 失败）/ kLimitExceeded（记录大于整环
    ///         或批进行中空间不足）
    Err EnsureRingSpace(uint64_t rec_bytes) noexcept {
        if (ring->HasRoomFor(rec_bytes)) return Err::kOk;
        if (batch_in_progress) return Err::kLimitExceeded;
        uint64_t covered = 0;
        const Err fe = commit->FlushNow(&covered);
        if (!IsOk(fe)) return fe;
        auto* ck = layout::AsAtomicU64(&region->super_block()->checkpoint_seq);
        uint64_t cur = ck->load();
        while (cur < covered && !ck->compare_exchange_weak(cur, covered)) {
        }
        ring->DropAllRetained();
        if (!ring->HasRoomFor(rec_bytes)) return Err::kLimitExceeded;
        return Err::kOk;
    }

    /// \brief A1 批前 WAL 空间预留：在批的首条记录（BEGIN）之前调用。
    ///
    /// 计算整批所需字节（BEGIN + N×行操作 + COMMIT）：
    ///  - 超过记录区容量 → kLimitExceeded 拒批（零 WAL 副作用，批整体
    ///    回滚、引擎不毒化）；
    ///  - 否则腾环（flush + 推进 checkpoint + 丢弃已覆盖记录——批前根锁
    ///    在手、环内不存在未提交前缀，丢弃安全），此后批内追加不再触发
    ///    覆盖协议，杜绝"环满中途丢弃未提交前缀"（P0-1）。
    ///
    /// 预留成功后置批进行中标志；批 WAL 段结束（无论成败）由调用方复位
    /// （见 batch_in_progress 注释）。
    ///
    /// \return kOk / kPoisoned / kIoError（flush 失败）/ kLimitExceeded
    Err ReserveBatchWal(uint64_t total_bytes) noexcept {
        if (ring == nullptr || ring->poisoned()) return Err::kPoisoned;
        if (total_bytes > ring->area_bytes()) return Err::kLimitExceeded;
        const Err e = EnsureRingSpace(total_bytes);
        if (!IsOk(e)) {
            if (e != Err::kLimitExceeded) Poison();
            return e;
        }
        batch_in_progress = true;
        return Err::kOk;
    }

    /// \brief 追加批标记/DDL 记录；失败按真实语义毒化（kLimitExceeded 除外）。
    Err WalAppendMark(uint8_t op, const uint8_t* payload, uint32_t payload_len,
                      uint64_t* out_seq) noexcept {
        if (ring == nullptr || ring->poisoned()) return Err::kPoisoned;
        const Err e = EnsureRingSpace(storage::WalRing::RecordBytes(payload_len));
        if (!IsOk(e)) {
            if (e != Err::kLimitExceeded) Poison();
            return e;
        }
        const Err ae = ring->AppendMark(op, payload, payload_len, out_seq);
        if (!IsOk(ae) && ae != Err::kLimitExceeded) Poison();
        return ae;
    }

    /// \brief 追加一条行操作记录（payload 取自暂存产物）。
    /// 注入点：`wal.append.fail`（错误注入：模拟追加失败 → 毒化）。
    ///
    /// 追加成功即置 `st.wal_written`（A2 所有权不变式）：记录已入环，
    /// 重放可能按记录内偏移重新物化行——此后本进程不得回收该操作的变长块。
    Err WalAppendRowOp(storage::TableStore* store, storage::StagedRowOp& st,
                       uint64_t* out_seq) noexcept {
        if (ring == nullptr || ring->poisoned()) return Err::kPoisoned;
        uint8_t op = storage::kWalOpInsert;
        const uint8_t* payload = nullptr;
        uint32_t payload_len = 0;
        switch (st.kind) {
            case storage::StagedRowOp::Kind::kInsert:
                op = storage::kWalOpInsert;
                payload = st.rec.data();
                payload_len = static_cast<uint32_t>(st.rec.size());
                break;
            case storage::StagedRowOp::Kind::kUpdate:
                op = storage::kWalOpUpdate;
                payload = st.rec.data();
                payload_len = static_cast<uint32_t>(st.rec.size());
                break;
            case storage::StagedRowOp::Kind::kDelete:
                op = storage::kWalOpDelete;
                payload = reinterpret_cast<const uint8_t*>(st.key);
                payload_len = 8;
                break;
            default:
                return Err::kInternal;
        }
        if (failpoint::ErrorPoint("wal.append.fail")) {
            Poison();
            return Err::kIoError;
        }
        const Err e = EnsureRingSpace(storage::WalRing::RecordBytes(payload_len));
        if (!IsOk(e)) {
            if (e != Err::kLimitExceeded) Poison();
            return e;
        }
        const storage::shm_TableDescriptor* desc = store->descriptor();
        const Err ae = ring->Append(op, static_cast<int64_t>(desc->table_id), st.row_no,
                                    st.row_ver, payload, payload_len, out_seq);
        if (IsOk(ae)) st.wal_written = true;  // A2 所有权转移点（记录已入环）
        if (!IsOk(ae) && ae != Err::kLimitExceeded) Poison();
        return ae;
    }
};

Engine::Impl::Impl() = default;

Engine::Impl::~Impl() {
    // 先停后台线程，再做优雅收尾（避免并发 flush）。
    commit.reset();
    if (region != nullptr && region->allocator() != nullptr) {
        auto* sb = region->super_block();
        const uint64_t committed = layout::AsAtomicU64(&sb->committed_seq)->load();
        // 优雅关闭统一做一次全映像 flush（never 档仅承诺运行期不主动刷，
        // 关闭收尾不属于"主动 flush"语义争议面，落盘利大于弊）。
        (void)region->FlushAll();
        auto* fs = layout::AsAtomicU64(&sb->flushed_seq);
        uint64_t cur = fs->load();
        while (cur < committed && !fs->compare_exchange_weak(cur, committed)) {
        }
    }
}

Engine::Engine() noexcept = default;

Engine::~Engine() = default;

Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Result<std::unique_ptr<Engine>> Engine::Open(const Options& opts) {
    if (opts.instance.empty() || opts.path.empty())
        return Result<std::unique_ptr<Engine>>::Fail(Err::kInvalidArgument);
    // 头部 + 元数据分区之后至少要有一点可分配空间。
    if (opts.initial_size_bytes < static_cast<uint64_t>(layout::kDataStartOffset) + 4096)
        return Result<std::unique_ptr<Engine>>::Fail(Err::kIncompatibleLayout);

    ShmRegionParams rp;
    rp.map_name = "rtdb_" + opts.instance;
    rp.file_path = opts.path;
    rp.size_bytes = opts.initial_size_bytes;
    switch (opts.role) {
        case Role::Writer:
            rp.mode = OpenMode::OpenExisting;  // 集群写者只能挂载既有映像
            break;
        case Role::Reader:
            rp.mode = OpenMode::OpenExisting;
            break;
        case Role::Standalone:
        default:
            rp.mode = OpenMode::OpenOrCreate;
            break;
    }
    rp.role = opts.role;
    rp.fsync = opts.wal_fsync;

    auto engine = std::unique_ptr<Engine>(new Engine());
    engine->impl_.reset(new Impl());
    engine->impl_->image_path = opts.path;
    auto region_res = ShmRegion::Open(rp);
    if (!region_res.IsOk()) return Result<std::unique_ptr<Engine>>::Fail(region_res.Error());
    engine->impl_->region = region_res.TakeValue();

    // 目录装配：写侧角色在目录未布局时初始化，Reader 仅复附着。
    auto* region = engine->impl_->region.get();
    layout::shm_SuperBlock* sb = region->super_block();
    const bool can_init_catalog = (region->allocator() != nullptr) && (sb->catalog_desc_off == 0);
    auto catalog_res =
        storage::Catalog::Attach(region->base_address(), region->allocator(), sb, can_init_catalog);
    if (!catalog_res.IsOk()) return Result<std::unique_ptr<Engine>>::Fail(catalog_res.Error());
    engine->impl_->catalog = catalog_res.TakeValue();
    if (can_init_catalog) region->Flush();  // 目录根块偏移与内容落盘

    // ---- M1-W4：WalRing 装配 + 恢复重放 + 组提交（写侧角色）----
    if (region->allocator() != nullptr) {
        bool created = false;
        auto ring_res = storage::WalRing::CreateOrAttach(region->base_address(),
                                                         region->allocator(), sb, &created);
        if (!ring_res.IsOk()) return Result<std::unique_ptr<Engine>>::Fail(ring_res.Error());
        engine->impl_->ring = ring_res.TakeValue();

        if (!created) {
            // 恢复路径 v1：扫描 → 批过滤 → 幂等重放 → 发布 committed_seq
            // （docs/08 §3-W4）。空环时为廉价 no-op。
            storage::ReplayStats st;
            const Err rerr =
                storage::ReplayWal(engine->impl_->catalog.get(), region->base_address(),
                                   region->allocator(), engine->impl_->ring.get(), sb, &st);
            if (!IsOk(rerr)) return Result<std::unique_ptr<Engine>>::Fail(rerr);
        }

        // ---- M1-W5-d：二级索引延迟重建（重放后全量自愈，docs/08 §3-W5）----
        // 在组提交装配前完成：重建只改索引树（根锁内），不影响环/水位。
        const Err rebuild_err = engine->impl_->RebuildSecondaryIndexes();
        if (!IsOk(rebuild_err)) return Result<std::unique_ptr<Engine>>::Fail(rebuild_err);

        engine->impl_->commit.reset(new storage::GroupCommit(region, sb, opts.wal_fsync));
        region->Flush();  // 环偏移/CRC 与恢复后的水位字段落盘
    }

    return Result<std::unique_ptr<Engine>>::Ok(std::move(engine));
}

int64_t Engine::CommittedSeq() const noexcept {
    return impl_ ? impl_->region->LoadCommittedSeq() : -1;
}

int64_t Engine::FlushedSeq() const noexcept {
    if (!impl_ || !impl_->region) return -1;
    return static_cast<int64_t>(
        layout::AsAtomicU64(&impl_->region->super_block()->flushed_seq)->load());
}

int64_t Engine::CheckpointSeq() const noexcept {
    if (!impl_ || !impl_->region) return -1;
    return static_cast<int64_t>(
        layout::AsAtomicU64(&impl_->region->super_block()->checkpoint_seq)->load());
}

int64_t Engine::SessionCount() const noexcept {
    // session_count 由 ShmRegion::Open 以原子 fetch_add 推进（并发 Open /
    // 多挂载者），此处必须走同一原子视角：plain 读与 atomic 写混用即
    // C++ 数据竞争（TSan 报点；与 FlushedSeq/CheckpointSeq 同款写法）。
    return impl_ ? layout::AsAtomicI64(&impl_->region->super_block()->session_count)->load() : -1;
}

TableHandle Engine::MakeHandle(int64_t table_id, const char* name, uint32_t layout_version,
                               uint32_t col_count, uint32_t ext_col_count, uint32_t row_cap,
                               uint64_t desc_off) {
    return TableHandle(table_id, std::string(name != nullptr ? name : ""), layout_version,
                       col_count, ext_col_count, row_cap, desc_off);
}

Result<TableHandle> Engine::CreateTable(const TableDef& def) {
    if (!impl_ || !impl_->catalog) return Result<TableHandle>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr)
        return Result<TableHandle>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<TableHandle>::Fail(Err::kPoisoned);

    // ---- M2：TableDef.secondary 前置校验（docs/10 §2-M2-D1）----
    // 列名在 def.columns 上解析（单列约束）；索引名格式与列类型由 store 层
    // CreateSecondaryIndex 复核；索引间重名在此拦截（kAlreadyExists）。
    for (const IndexDef& idx : def.secondary) {
        if (idx.columns.size() != 1) return Result<TableHandle>::Fail(Err::kUnsupported);
        bool found = false;
        for (const ColumnDef& col : def.columns) {
            if (col.name == idx.columns[0]) {
                found = true;
                break;
            }
        }
        if (!found) return Result<TableHandle>::Fail(Err::kInvalidArgument);
        for (const IndexDef& other : def.secondary) {
            if (&other != &idx && other.name == idx.name)
                return Result<TableHandle>::Fail(Err::kAlreadyExists);
        }
    }

    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<TableHandle>::Fail(guard.error());

    Result<uint64_t> desc_res = impl_->catalog->CreateTable(def);
    if (!desc_res.IsOk()) return Result<TableHandle>::Fail(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
    if (desc == nullptr) return Result<TableHandle>::Fail(Err::kCorruption);

    // DDL 入环（M1-W4 仅观测：重放还原归 W5/M2；目录事务本身按
    // "先写描述符再挂树" 顺序崩溃安全，docs/03 §4.1）。失败毒化引擎，
    // 但建表已成功，仍返回句柄（毒化状态由下一次写操作显式暴露）。
    if (impl_->ring != nullptr && !impl_->ring->poisoned()) {
        const auto* name = reinterpret_cast<const uint8_t*>(desc->name);
        uint32_t name_len = 0;
        while (name_len < storage::kMaxTableNameBytes && desc->name[name_len] != '\0') ++name_len;
        uint64_t ddl_seq = 0;
        const Err werr =
            impl_->WalAppendMark(storage::kWalOpCreateTable, name, name_len, &ddl_seq);
        if (IsOk(werr)) {
            impl_->MarkCommitted(ddl_seq);
        } else if (werr != Err::kPoisoned) {
            impl_->Poison();
        }
    }

    // ---- M2：建表直建二级索引（TableDef.secondary 便利面）----
    // 崩溃安全序与 CreateSecondaryIndex 相同（分配 → populate → 挂链，
    // 不写 WAL，由写侧 Open 全量重建兜底）；失败回滚建表（墓碑 + 摘名，
    // 物理区泄漏同 DropTable 既有语义，待 compact）。
    for (const IndexDef& idx : def.secondary) {
        uint32_t col_index = 0;
        for (size_t i = 0; i < def.columns.size(); ++i) {
            if (def.columns[i].name == idx.columns[0]) {
                col_index = static_cast<uint32_t>(i);
                break;
            }
        }
        SecondaryIndexDef sidx;
        sidx.name = std::string(idx.name);
        sidx.column_index = col_index;
        sidx.unique = idx.unique;
        auto store_res = impl_->StoreFor(desc_off, static_cast<int64_t>(desc->table_id));
        if (!store_res.IsOk()) {
            impl_->catalog->DropTable(def.name);
            return Result<TableHandle>::Fail(store_res.Error());
        }
        const Err ierr = store_res.Value()->CreateSecondaryIndex(sidx);
        if (!IsOk(ierr)) {
            impl_->catalog->DropTable(def.name);
            return Result<TableHandle>::Fail(ierr);
        }
    }

    TableHandle handle = MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                    desc->data_layout_ver, desc->col_count, desc->ext_col_count,
                                    desc->row_cap, desc_off);
    handle.set_engine(this);
    return Result<TableHandle>::Ok(std::move(handle));
}

Result<TableHandle> Engine::OpenTable(std::string_view name) {
    if (!impl_ || !impl_->catalog) return Result<TableHandle>::Fail(Err::kUnsupported);

    // 读路径根锁（M3 TSan 加固，docs/13 §8-W5）：目录/表索引的遍历会与
    // CreateTable/DropTable/AlterTableAddColumn 等写路径并发访问同一 B+ 树；
    // 原"免锁近似读"在 C++ 内存模型下构成数据竞争（TSan：SharedBTree::Insert
    // vs 读侧遍历）。写侧角色统一在根锁内读，语义从"近似"提升为"与写互斥"。
    // Reader 角色无根锁（只读映射、跨进程读；TSan 不跨进程跟踪，见 ci.yml）。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked()) return Result<TableHandle>::Fail(guard.error());

    Result<uint64_t> desc_res = impl_->catalog->OpenTable(name);
    if (!desc_res.IsOk()) return Result<TableHandle>::Fail(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
    if (desc == nullptr) return Result<TableHandle>::Fail(Err::kCorruption);
    TableHandle handle = MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                    desc->data_layout_ver, desc->col_count, desc->ext_col_count,
                                    desc->row_cap, desc_off);
    handle.set_engine(this);
    return Result<TableHandle>::Ok(std::move(handle));
}

Result<void> Engine::DropTable(std::string_view name) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<void>::Fail(Err::kPoisoned);
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    const Err err = impl_->catalog->DropTable(name);
    if (!IsOk(err)) return Result<void>::Fail(err);

    // DDL 入环（观测语义同 CreateTable）。
    if (impl_->ring != nullptr && !impl_->ring->poisoned()) {
        std::string n(name.substr(0, storage::kMaxTableNameBytes - 1));
        uint64_t ddl_seq = 0;
        const Err werr = impl_->WalAppendMark(storage::kWalOpDropTable,
                                              reinterpret_cast<const uint8_t*>(n.data()),
                                              static_cast<uint32_t>(n.size()), &ddl_seq);
        if (IsOk(werr)) {
            impl_->MarkCommitted(ddl_seq);
        } else if (werr != Err::kPoisoned) {
            impl_->Poison();
        }
    }
    return Result<void>::Ok();
}

// ------------------ 行 CRUD（WAL 先行协议，docs/08 §2-D2）------------------

Result<uint64_t> Engine::Insert(const TableHandle& table, const std::vector<ColumnValue>& row) {
    if (!impl_ || !impl_->catalog) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<uint64_t>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<uint64_t>::Fail(Err::kReadOnlyHandle);
    const Err width_err = impl_->CheckRowWidth(table, row.size());
    if (!IsOk(width_err)) return Result<uint64_t>::Fail(width_err);

    uint64_t commit_seq = 0;
    Result<uint64_t> res = Result<uint64_t>::Fail(Err::kInternal);
    {
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<uint64_t>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<uint64_t>::Fail(Err::kPoisoned);

        auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
        if (!store_res.IsOk()) return Result<uint64_t>::Fail(store_res.Error());
        storage::TableStore* store = store_res.Value();

        // 注入点：Insert 前（docs/06 §3）。
        failpoint::CrashPoint("wal.insert.before");

        storage::StagedRowOp st;
        bool staged = false;  // 暂存成功才允许释放（默认构造对象禁入 ReleaseStaged）
        Err err = store->StageInsert(row, &st);
        if (IsOk(err)) {
            staged = true;
            err = impl_->WalAppendRowOp(store, st, &commit_seq);  // WAL 先行
            if (IsOk(err)) {
                err = store->ApplyStaged(&st);
                if (!IsOk(err)) impl_->Poison();  // 应用失败：重启重放收敛
            }
        }
        if (!IsOk(err)) {
            if (staged) store->ReleaseStaged(&st);
            res = Result<uint64_t>::Fail(err);
        } else {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            res = Result<uint64_t>::Ok(st.row_no);
        }
    }
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
    // always 档 flush 失败/超时 = 持久化承诺破裂 → 毒化拒写。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

// ---- M2：upsert 便利面（docs/04 §3 `tbl.Upsert(Row)`，docs/10 §2-M2-D1）----
// 语义：存在则按所给列**合并**覆盖（未提供列保持原值），不存在则插入
// （未提供列按 schema 填充：nullable → NULL；非空且无默认值 →
// kInvalidArgument）。
//
// 原子性（P0-1 修复）：存在性判定、当前行读取、合并、WAL 与发布全部在
// **同一个根锁临界区**内完成。原实现分别调用 ContainsKey/GetByKey/
// UpdateByKey（各自取锁），两个会话并发改同一主键的不同列时会互相覆盖
// （丢更新）。合并路径现在对同一主键等价于一次行级事务。
Result<uint64_t> Engine::Upsert(const TableHandle& table, const Row& row) {
    if (!impl_ || !impl_->catalog) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<uint64_t>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<uint64_t>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<uint64_t>::Fail(Err::kPoisoned);
    if (row.empty()) return Result<uint64_t>::Fail(Err::kInvalidArgument);

    uint64_t commit_seq = 0;
    Result<uint64_t> res = Result<uint64_t>::Fail(Err::kInternal);
    {
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<uint64_t>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<uint64_t>::Fail(Err::kPoisoned);

        // 描述符解析与元数据收集必须在根锁内：与 DropTable/AlterTable 互斥，
        // 避免在锁外持有裸描述符指针（P0-2 同类问题）。
        auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
        if (!store_res.IsOk()) return Result<uint64_t>::Fail(store_res.Error());
        storage::TableStore* store = store_res.Value();
        const storage::shm_TableDescriptor* desc = store->descriptor();
        const uint32_t visible = table.visible_column_count();

        std::vector<ColInfo> meta;
        const Err meta_err =
            CollectColumnInfo(impl_->region->base_address(), desc, visible, &meta);
        if (!IsOk(meta_err)) return Result<uint64_t>::Fail(meta_err);

        // ---- Row 校验：列名须在可见集内、不得重名、主键列（列 0）必给 ----
        std::vector<const ColumnValue*> provided(visible, nullptr);
        for (const auto& kv : row) {
            uint32_t idx = visible;
            for (uint32_t i = 0; i < visible; ++i) {
                if (kv.first == meta[i].name) {
                    idx = i;
                    break;
                }
            }
            if (idx == visible || provided[idx] != nullptr)
                return Result<uint64_t>::Fail(Err::kInvalidArgument);
            provided[idx] = &kv.second;
        }
        if (provided[0] == nullptr) return Result<uint64_t>::Fail(Err::kInvalidArgument);
        const ColumnValue& pk = *provided[0];

        // ---- 存在性分流（锁内）----
        auto exists_res = store->Contains(pk);
        if (!exists_res.IsOk()) return Result<uint64_t>::Fail(exists_res.Error());
        const bool exists = exists_res.Value();

        std::vector<ColumnValue> full(visible);
        if (exists) {
            // 合并：现存行 + 所给列覆盖（锁内读，保证读到的就是本次覆盖的基线）。
            auto cur_res = store->Get(pk);
            if (!cur_res.IsOk()) return Result<uint64_t>::Fail(cur_res.Error());
            std::vector<ColumnValue> cur = cur_res.TakeValue();
            if (cur.size() < visible) return Result<uint64_t>::Fail(Err::kCorruption);
            if (cur.size() > visible)
                cur.erase(cur.begin() + static_cast<std::ptrdiff_t>(visible), cur.end());
            for (uint32_t i = 0; i < visible; ++i)
                full[i] = provided[i] != nullptr ? *provided[i] : cur[i];
        } else {
            // 插入：缺列按 schema 填充（ext 列默认值经 EncodeExtDefault 的逆解码）
            for (uint32_t i = 0; i < visible; ++i) {
                if (provided[i] != nullptr) {
                    full[i] = *provided[i];
                    continue;
                }
                if (i < desc->col_count) {
                    if (meta[i].nullable) {
                        full[i] = ColumnValue::Null();
                    } else {
                        return Result<uint64_t>::Fail(Err::kInvalidArgument);
                    }
                    continue;
                }
                if (meta[i].has_default) {
                    full[i] = DecodeExtDefault(meta[i]);
                } else if (meta[i].nullable) {
                    full[i] = ColumnValue::Null();
                } else {
                    return Result<uint64_t>::Fail(Err::kInvalidArgument);
                }
            }
            failpoint::CrashPoint("wal.insert.before");
        }

        storage::StagedRowOp st;
        bool staged = false;
        Err err = exists ? store->StageUpdate(pk, full, &st) : store->StageInsert(full, &st);
        if (IsOk(err)) {
            staged = true;
            err = impl_->WalAppendRowOp(store, st, &commit_seq);
            if (IsOk(err)) {
                err = store->ApplyStaged(&st);
                if (!IsOk(err)) impl_->Poison();
            }
        }
        if (!IsOk(err)) {
            if (staged) store->ReleaseStaged(&st);
            res = Result<uint64_t>::Fail(err);
        } else {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            res = Result<uint64_t>::Ok(st.row_no);
        }
    }
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

// ---- M2-W2：列式块原子 upsert（docs/03 §2.3，docs/10 §2-M2-D2）----
// 与单行 Upsert 的"按列合并"不同：块语义为**整行覆盖**（批量入口不做
// 读-改-写，未提供列按 schema 填充）。三阶段结构同 ExecuteBatch：
// 全量暂存（零副作用）→ WAL 三段式（COMMIT 先于数据页）→ 统一应用。
Result<uint64_t> Engine::UpsertBatch(const RecordBlockView& block) {
    if (!impl_ || !impl_->catalog) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr) return Result<uint64_t>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<uint64_t>::Fail(Err::kPoisoned);
    if (block.table_name.empty() || block.columns.empty() || block.row_count == 0)
        return Result<uint64_t>::Fail(Err::kInvalidArgument);

    // ---- 视图自校验：列名非空不重名、行数一致、载荷类型合法 ----
    for (size_t i = 0; i < block.columns.size(); ++i) {
        const BlockColumn& c = block.columns[i];
        if (c.name.empty() || c.data == nullptr || c.rows != block.row_count)
            return Result<uint64_t>::Fail(Err::kInvalidArgument);
        if (c.type != DataType::kInt64 && c.type != DataType::kFloat64 &&
            c.type != DataType::kUtf8Var)
            return Result<uint64_t>::Fail(Err::kInvalidArgument);
        for (size_t j = 0; j < i; ++j) {
            if (block.columns[j].name == c.name)
                return Result<uint64_t>::Fail(Err::kInvalidArgument);
        }
    }

    uint64_t commit_seq = 0;
    Result<uint64_t> res = Result<uint64_t>::Fail(Err::kInternal);
    {
        // P0-2 修复：描述符解析、扩展列链遍历与列映射全部在根锁内完成。
        // 原实现先在锁外取 `desc` 裸指针再进锁，与 DropTable/AlterTable
        // 并发时可能读到墓碑或已摘除的扩展列区域。
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<uint64_t>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<uint64_t>::Fail(Err::kPoisoned);

        // ---- 解析表（按名）与列映射 ----
        Result<uint64_t> desc_res = impl_->catalog->OpenTable(block.table_name);
        if (!desc_res.IsOk()) return Result<uint64_t>::Fail(desc_res.Error());
        const uint64_t desc_off = desc_res.Value();
        const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
        if (desc == nullptr) return Result<uint64_t>::Fail(Err::kCorruption);
        if (desc->state != static_cast<uint32_t>(storage::TableState::kActive))
            return Result<uint64_t>::Fail(Err::kTableNotFound);

        const uint32_t total_cols = desc->col_count + desc->ext_col_count;
        if (total_cols == 0) return Result<uint64_t>::Fail(Err::kCorruption);
        std::vector<ColInfo> meta;
        const Err meta_err =
            CollectColumnInfo(impl_->region->base_address(), desc, total_cols, &meta);
        if (!IsOk(meta_err)) return Result<uint64_t>::Fail(meta_err);

        std::vector<int32_t> src(total_cols, -1);  // schema 列 → block 列下标
        for (uint32_t c = 0; c < total_cols; ++c) {
            for (size_t b = 0; b < block.columns.size(); ++b) {
                if (block.columns[b].name == meta[c].name) {
                    if (!PayloadTypeCompatible(block.columns[b].type, meta[c].type))
                        return Result<uint64_t>::Fail(Err::kInvalidArgument);
                    src[c] = static_cast<int32_t>(b);
                    break;
                }
            }
        }
        for (size_t b = 0; b < block.columns.size(); ++b) {  // 块内未知列名
            bool mapped = false;
            for (uint32_t c = 0; c < total_cols; ++c) {
                if (src[c] == static_cast<int32_t>(b)) {
                    mapped = true;
                    break;
                }
            }
            if (!mapped) return Result<uint64_t>::Fail(Err::kInvalidArgument);
        }

        auto store_res = impl_->StoreFor(desc_off, static_cast<int64_t>(desc->table_id));
        if (!store_res.IsOk()) return Result<uint64_t>::Fail(store_res.Error());
        storage::TableStore* store = store_res.Value();

        // ---- 阶段一：逐行解码（缓冲复用，零逐行堆分配）+ 暂存 ----
        // 单表批：store 对整块唯一；行值与暂存缓冲跨批复用（Impl 高水位
        // 保留，见 Impl::block_staged 注释）。
        std::vector<ColumnValue>& vals = impl_->block_vals;
        vals.resize(total_cols);
        std::vector<storage::StagedRowOp>& staged = impl_->block_staged;
        staged.resize(block.row_count);
        size_t staged_n = 0;
        std::unordered_set<int64_t> seen_pk;
        seen_pk.reserve(block.row_count);
        Err phase_err = Err::kOk;
        for (size_t r = 0; r < block.row_count && IsOk(phase_err); ++r) {
            for (uint32_t c = 0; c < total_cols; ++c) {
                if (src[c] < 0) {
                    if (meta[c].has_default) {
                        vals[c] = DecodeExtDefault(meta[c]);
                    } else if (meta[c].nullable) {
                        vals[c] = ColumnValue::Null();
                    } else {
                        phase_err = Err::kInvalidArgument;  // 非空缺列未提供
                        break;
                    }
                    continue;
                }
                const BlockColumn& bc = block.columns[static_cast<size_t>(src[c])];
                if (bc.null_flags != nullptr && bc.null_flags[r] != 0) {
                    if (!meta[c].nullable) {
                        phase_err = Err::kInvalidArgument;  // NOT NULL 列不可空
                        break;
                    }
                    vals[c] = ColumnValue::Null();
                    continue;
                }
                switch (bc.type) {
                    case DataType::kInt64:
                        vals[c] = ColumnValue::IntOf(
                            meta[c].type, static_cast<const int64_t*>(bc.data)[r]);
                        break;
                    case DataType::kFloat64:
                        vals[c] = ColumnValue::Real(static_cast<const double*>(bc.data)[r]);
                        break;
                    case DataType::kUtf8Var:
                        vals[c] = ColumnValue::Text(
                            std::string(static_cast<const std::string_view*>(bc.data)[r]));
                        break;
                    default:
                        phase_err = Err::kInvalidArgument;
                        break;
                }
                if (!IsOk(phase_err)) break;
            }
            if (!IsOk(phase_err)) break;

            // 块内主键不得重复（同批同键两次暂存语义不确定）。
            if (!seen_pk.insert(vals[0].as_int()).second) {
                phase_err = Err::kInvalidArgument;
                break;
            }

            auto has = store->Contains(vals[0]);
            if (!has.IsOk()) {
                phase_err = has.Error();
                break;
            }
            storage::StagedRowOp& st = staged[staged_n];
            phase_err = has.Value() ? store->StageUpdate(vals[0], vals, &st)
                                    : store->StageInsert(vals, &st);
            if (!IsOk(phase_err)) break;
            ++staged_n;
        }
        if (!IsOk(phase_err)) {
            for (size_t j = staged_n; j > 0; --j) store->ReleaseStaged(&staged[j - 1]);
            return Result<uint64_t>::Fail(phase_err);
        }

        // ---- 阶段二：WAL 三段式（BEGIN + N×op + COMMIT 先于数据页变更）----
        // A1 批前预留（P0-1 修复）：整批字节 = BEGIN + N×行操作 + COMMIT。
        // 环空间不足时在此一次性腾净（flush + checkpoint + 丢弃已提交段，
        // 批前环内不存在未提交前缀，丢弃安全）；整批超过记录区容量直接
        // 拒批（kLimitExceeded，零 WAL 副作用、批整体回滚、不毒化）。
        uint64_t batch_wal_bytes =
            storage::WalRing::RecordBytes(4) + storage::WalRing::RecordBytes(0);
        for (size_t i = 0; i < staged_n; ++i) batch_wal_bytes += StagedWalBytes(staged[i]);
        phase_err = impl_->ReserveBatchWal(batch_wal_bytes);

        bool begin_written = false;
        uint8_t cnt_buf[4];
        storage::EncodeU32LE(cnt_buf, static_cast<uint32_t>(staged_n));
        uint64_t begin_seq = 0;
        if (IsOk(phase_err)) {
            phase_err = impl_->WalAppendMark(storage::kWalOpBatchBegin, cnt_buf, 4, &begin_seq);
        }
        if (IsOk(phase_err)) begin_written = true;
        for (size_t i = 0; IsOk(phase_err) && i < staged_n; ++i) {
            phase_err = impl_->WalAppendRowOp(store, staged[i], nullptr);
            if (IsOk(phase_err)) {
                // 注入点：块 WAL 阶段中断（无 COMMIT → 恢复整批作废）。
                failpoint::CrashPoint("wal.batch.wal_mid");
            }
        }
        // 注入点：COMMIT 落环前（块级确定性变体，docs/10 §2-M2-D2）。
        if (IsOk(phase_err)) failpoint::CrashPoint("block.commit.before");
        if (IsOk(phase_err)) {
            phase_err = impl_->WalAppendMark(storage::kWalOpBatchCommit, nullptr, 0, &commit_seq);
        }
        impl_->batch_in_progress = false;  // 批 WAL 段结束（无论成败）
        if (!IsOk(phase_err) && begin_written) impl_->Poison();

        // ---- 阶段三：应用（COMMIT 已落环 → 崩溃由重放整批收敛）----
        size_t applied = 0;
        if (IsOk(phase_err)) {
            for (; applied < staged_n; ++applied) {
                const Err aerr = store->ApplyStaged(&staged[applied]);
                if (!IsOk(aerr)) {
                    phase_err = aerr;
                    impl_->Poison();
                    break;
                }
                // 注入点：块应用中途（首行已应用，其余未应用）。
                if (applied == 0) failpoint::CrashPoint("block.apply.mid");
            }
        }

        if (IsOk(phase_err)) {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            res = Result<uint64_t>::Ok(commit_seq);
        } else {
            // 回滚未应用的暂存（已应用项 allocated_vars 已清空，跳过）。
            for (size_t j = staged_n; j > applied; --j) store->ReleaseStaged(&staged[j - 1]);
            res = Result<uint64_t>::Fail(phase_err);
        }
    }
    // flush 在根锁之外流水线化（组提交聚合）。
    // always 档 flush 失败/超时 = 持久化承诺破裂 → 毒化拒写。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

Result<std::vector<ColumnValue>> Engine::GetByKey(const TableHandle& table,
                                                  const ColumnValue& key) {
    if (!impl_) return Result<std::vector<ColumnValue>>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<std::vector<ColumnValue>>::Fail(Err::kInvalidArgument);

    // 读路径根锁（M3 TSan 加固）：主键索引 find 与行记录读取和写路径互斥，
    // 消除免锁遍历/行拷贝与并发插入更新的数据竞争；行版本奇偶协议保留为
    // 锁内一致性校验（Reader 角色无根锁时仍以该协议兜底近似读）。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked())
        return Result<std::vector<ColumnValue>>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<std::vector<ColumnValue>>::Fail(store_res.Error());
    auto got = store_res.Value()->Get(key);  // 根锁内读（全量当前 schema）
    if (!got.IsOk()) return Result<std::vector<ColumnValue>>::Fail(got.Error());
    std::vector<ColumnValue> values = got.TakeValue();
    // 旧句柄只看到绑定时刻的列（加列只追加，前缀截断即旧 schema 视图）。
    // 用 erase 而非 resize：ColumnValue 默认构造私有（工厂函数模式），
    // resize 的实例化路径需要默认构造。
    const uint32_t visible = table.visible_column_count();
    if (values.size() > visible)
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(visible), values.end());
    return Result<std::vector<ColumnValue>>::Ok(std::move(values));
}

Result<void> Engine::UpdateByKey(const TableHandle& table, const ColumnValue& key,
                                 const std::vector<ColumnValue>& row, uint64_t* out_row_no) {
    if (out_row_no != nullptr) *out_row_no = 0;
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);
    const Err width_err = impl_->CheckRowWidth(table, row.size());
    if (!IsOk(width_err)) return Result<void>::Fail(width_err);

    uint64_t commit_seq = 0;
    Result<void> res = Result<void>::Fail(Err::kInternal);
    {
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<void>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<void>::Fail(Err::kPoisoned);

        auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
        if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());
        storage::TableStore* store = store_res.Value();

        storage::StagedRowOp st;
        bool staged = false;  // 暂存成功才允许释放（默认构造对象禁入 ReleaseStaged）
        Err err = store->StageUpdate(key, row, &st);
        if (IsOk(err)) {
            staged = true;
            err = impl_->WalAppendRowOp(store, st, &commit_seq);
            if (IsOk(err)) {
                err = store->ApplyStaged(&st);
                if (!IsOk(err)) impl_->Poison();
            }
        }
        if (!IsOk(err)) {
            if (staged) store->ReleaseStaged(&st);
            res = Result<void>::Fail(err);
        } else {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            if (out_row_no != nullptr) *out_row_no = st.row_no;
            res = Result<void>::Ok();
        }
    }
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
    // always 档 flush 失败/超时 = 持久化承诺破裂 → 毒化拒写。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

Result<void> Engine::DeleteByKey(const TableHandle& table, const ColumnValue& key) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);

    uint64_t commit_seq = 0;
    Result<void> res = Result<void>::Fail(Err::kInternal);
    {
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<void>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<void>::Fail(Err::kPoisoned);

        auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
        if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());
        storage::TableStore* store = store_res.Value();

        storage::StagedRowOp st;
        bool staged = false;  // 暂存成功才允许释放（默认构造对象禁入 ReleaseStaged）
        Err err = store->StageRemove(key, &st);
        if (IsOk(err)) {
            staged = true;
            err = impl_->WalAppendRowOp(store, st, &commit_seq);
            if (IsOk(err)) {
                err = store->ApplyStaged(&st);
                if (!IsOk(err)) impl_->Poison();
            }
        }
        if (!IsOk(err)) {
            if (staged) store->ReleaseStaged(&st);
            res = Result<void>::Fail(err);
        } else {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            res = Result<void>::Ok();
        }
    }
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
    // always 档 flush 失败/超时 = 持久化承诺破裂 → 毒化拒写。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

Result<bool> Engine::ContainsKey(const TableHandle& table, const ColumnValue& key) {
    if (!impl_) return Result<bool>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<bool>::Fail(Err::kInvalidArgument);

    // 读路径根锁（M3 TSan 加固）：与写路径互斥读取主键索引/墓碑位图。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked()) return Result<bool>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<bool>::Fail(store_res.Error());
    return store_res.Value()->Contains(key);
}

// ------------------ 范围扫描（M1-W5，docs/03 §4.2）------------------

Result<std::vector<std::vector<ColumnValue>>> Engine::Scan(const TableHandle& table,
                                                           const KeyRange& range) {
    return ScanLimited(table, range, static_cast<size_t>(UINT64_MAX), nullptr);
}

Result<std::vector<std::vector<ColumnValue>>> Engine::ScanLimited(const TableHandle& table,
                                                                  const KeyRange& range,
                                                                  size_t max_rows,
                                                                  bool* out_more) {
    if (out_more != nullptr) *out_more = false;
    if (!impl_ || !impl_->catalog) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                               Err::kUnsupported);
    if (!table.IsValid())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kInvalidArgument);

    // 读路径根锁（M3 TSan 加固）：范围扫描遍历主键 B+ 树叶链并逐行拷贝，
    // 必须与写路径互斥——原免锁遍历与并发插入（ShiftEntries/SplitChild）
    // 构成数据竞争（TSan：SharedBTree::Insert vs LoadAndCheckHi）。
    // 单次 Scan 调用内的结果因此与写互斥（Reader 跨进程角色仍为近似视图）。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                       store_res.Error());

    auto res = store_res.Value()->ScanLimited(
        range.lo.is_null() ? nullptr : &range.lo, range.lo_inclusive,
        range.hi.is_null() ? nullptr : &range.hi, range.hi_inclusive,
        static_cast<uint64_t>(max_rows), out_more);
    if (!res.IsOk()) return Result<std::vector<std::vector<ColumnValue>>>::Fail(res.Error());

    // 旧句柄列截断（同 GetByKey：加列只追加，前缀即旧 schema 视图）。
    std::vector<std::vector<ColumnValue>> rows = res.TakeValue();
    const uint32_t visible = table.visible_column_count();
    for (auto& row : rows) {
        if (row.size() > visible)
            row.erase(row.begin() + static_cast<std::ptrdiff_t>(visible), row.end());
    }
    return Result<std::vector<std::vector<ColumnValue>>>::Ok(std::move(rows));
}

Result<std::vector<std::vector<ColumnValue>>> Engine::ScanAll(const TableHandle& table) {
    KeyRange all;  // 两端均 Null = 无界
    return Scan(table, all);
}

// ------------------ 原子批量（docs/08 §2-D3 三段式）------------------

Result<void> Engine::ExecuteBatch(const std::vector<Engine::BatchOp>& ops) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (ops.empty()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);

    struct StagedEntry {
        storage::TableStore* store;
        storage::StagedRowOp st;
    };

    uint64_t commit_seq = 0;
    Result<void> res = Result<void>::Fail(Err::kInternal);
    {
        infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
        if (!guard.locked()) return Result<void>::Fail(guard.error());
        if (impl_->ring == nullptr || impl_->ring->poisoned())
            return Result<void>::Fail(Err::kPoisoned);

        // 阶段一：全部暂存（校验基于批前状态；任一失败零副作用）。
        std::vector<StagedEntry> staged;
        staged.reserve(ops.size());
        Err phase_err = Err::kOk;
        for (const BatchOp& op : ops) {
            auto store_res = impl_->StoreFor(op.table.desc_off(), op.table.table_id());
            if (!store_res.IsOk()) {
                phase_err = store_res.Error();
                break;
            }
            // 行宽校验（W5-b）：insert/update 按句柄可见列数。
            if (op.type == BatchOp::Type::kInsert || op.type == BatchOp::Type::kUpdate) {
                const Err we = impl_->CheckRowWidth(op.table, op.row.size());
                if (!IsOk(we)) {
                    phase_err = we;
                    break;
                }
            }
            storage::TableStore* store = store_res.Value();
            storage::StagedRowOp st;
            switch (op.type) {
                case BatchOp::Type::kInsert:
                    phase_err = store->StageInsert(op.row, &st);
                    break;
                case BatchOp::Type::kUpdate:
                    phase_err = store->StageUpdate(
                        op.row.empty() ? ColumnValue::Null() : op.row[0], op.row, &st);
                    break;
                case BatchOp::Type::kDelete:
                    phase_err = store->StageRemove(
                        op.row.empty() ? ColumnValue::Null() : op.row[0], &st);
                    break;
                default:
                    phase_err = Err::kInvalidArgument;
                    break;
            }
            if (!IsOk(phase_err)) break;
            staged.push_back(StagedEntry{store, std::move(st)});
        }
        if (!IsOk(phase_err)) {
            for (auto it = staged.rbegin(); it != staged.rend(); ++it)
                it->store->ReleaseStaged(&it->st);
            return Result<void>::Fail(phase_err);
        }

        // 阶段二：WAL 三段式（BEGIN + N×op + COMMIT 先于任何数据页变更）。
        // A1 批前预留（P0-1 修复）：先算整批字节并一次性腾环/拒批，批内追加
        // 不再触发覆盖协议（BEGIN 之后丢弃前缀会破坏批原子性）。
        // 注意：BEGIN 落环后若任何后续环节失败，环内将残留未决批——
        // 恢复扫描会把它之后的一切记录并入该批作废，因此必须毒化拒写。
        uint64_t batch_wal_bytes =
            storage::WalRing::RecordBytes(4) + storage::WalRing::RecordBytes(0);
        for (const StagedEntry& e : staged) batch_wal_bytes += StagedWalBytes(e.st);
        phase_err = impl_->ReserveBatchWal(batch_wal_bytes);

        bool begin_written = false;
        uint8_t cnt_buf[4];
        storage::EncodeU32LE(cnt_buf, static_cast<uint32_t>(ops.size()));
        uint64_t begin_seq = 0;
        if (IsOk(phase_err)) {
            phase_err = impl_->WalAppendMark(storage::kWalOpBatchBegin, cnt_buf, 4, &begin_seq);
        }
        if (IsOk(phase_err)) begin_written = true;
        for (size_t i = 0; IsOk(phase_err) && i < staged.size(); ++i) {
            phase_err = impl_->WalAppendRowOp(staged[i].store, staged[i].st, nullptr);
            if (IsOk(phase_err)) {
                // 注入点：批 WAL 阶段中断（无 COMMIT → 恢复整批作废）。
                failpoint::CrashPoint("wal.batch.wal_mid");
            }
        }
        if (IsOk(phase_err)) {
            phase_err =
                impl_->WalAppendMark(storage::kWalOpBatchCommit, nullptr, 0, &commit_seq);
        }
        impl_->batch_in_progress = false;  // 批 WAL 段结束（无论成败）
        if (!IsOk(phase_err) && begin_written) impl_->Poison();

        // 阶段三：应用（COMMIT 已落环 → 崩溃由重放整批收敛）。
        size_t applied = 0;
        if (IsOk(phase_err)) {
            for (; applied < staged.size(); ++applied) {
                const Err aerr = staged[applied].store->ApplyStaged(&staged[applied].st);
                if (!IsOk(aerr)) {
                    phase_err = aerr;
                    impl_->Poison();
                    break;
                }
                // 注入点：批应用阶段中断（有 COMMIT → 重放整批收敛）。
                failpoint::CrashPoint("wal.batch.mid");
            }
        }

        if (IsOk(phase_err)) {
            failpoint::CrashPoint("wal.commit.before");
            impl_->MarkCommitted(commit_seq);
            res = Result<void>::Ok();
        } else {
            // 回滚未应用的暂存（应用失败项的数据页已由 ApplyStaged 回滚；
            // 已应用项的 allocated_vars 已清空，跳过）。逆序释放行槽保留。
            for (size_t j = staged.size(); j > applied; --j)
                staged[j - 1].store->ReleaseStaged(&staged[j - 1].st);
            res = Result<void>::Fail(phase_err);
        }
    }
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
    // always 档 flush 失败/超时 = 持久化承诺破裂 → 毒化拒写。
    if (res.IsOk() && impl_->commit != nullptr && commit_seq != 0) {
        const Err fe = impl_->commit->OnCommit(commit_seq);
        if (!IsOk(fe)) impl_->Poison();
    }
    return res;
}

// ------------------ 在线加列（M1-W5，docs/02 §3 要点 3）------------------

Result<void> Engine::AlterTableAddColumn(const TableHandle& table, const ColumnDef& col) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<void>::Fail(Err::kPoisoned);

    // 根锁内完成"写区域 → 挂链 → 计数递增"（崩溃安全序，docs/08 §2-D5）。
    // 加列不写 WAL：描述符事务顺序即崩溃安全（同 CreateTable 观测语义），
    // 环内行记录的重放对先于加列的记录无后缀、对晚于加列的记录带后缀，
    // 两种形态均由 ReplayUpsert 兼容处理。
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());
    storage::TableStore* store = store_res.Value();

    const Err err =
        storage::AppendExtColumn(store->descriptor(), impl_->region->base_address(),
                                 impl_->region->allocator(), col);
    if (!IsOk(err)) return Result<void>::Fail(err);
    return Result<void>::Ok();
}

// ------------------ 二级索引（M1-W5，docs/03 §2.2）------------------

Result<void> Engine::CreateSecondaryIndex(const TableHandle& table, const SecondaryIndexDef& idx) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);
    if (impl_->ring != nullptr && impl_->ring->poisoned())
        return Result<void>::Fail(Err::kPoisoned);

    // 根锁内完成"分配 → populate → 挂链"（崩溃安全序，docs/08 §3-W5）。
    // 不写 WAL：描述符事务顺序即崩溃安全（同加列观测语义），索引本身
    // 由写侧 Open 的全量重建自愈。
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());

    const Err err = store_res.Value()->CreateSecondaryIndex(idx);
    if (!IsOk(err)) return Result<void>::Fail(err);
    return Result<void>::Ok();
}

Result<std::vector<std::vector<ColumnValue>>> Engine::GetBySecondary(const TableHandle& table,
                                                                     const std::string& index_name,
                                                                     const ColumnValue& key) {
    if (!impl_) return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kUnsupported);
    if (!table.IsValid())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kInvalidArgument);

    // 读路径根锁（M3 TSan 加固）：二级索引 B+ 树等值遍历与写路径互斥。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(store_res.Error());

    // 根锁内等值查（行记录仍按版本协议拷贝）。
    auto res = store_res.Value()->GetBySecondary(index_name, key);
    if (!res.IsOk()) return Result<std::vector<std::vector<ColumnValue>>>::Fail(res.Error());

    // 旧句柄列截断（同 GetByKey：加列只追加，前缀即旧 schema 视图）。
    std::vector<std::vector<ColumnValue>> rows = res.TakeValue();
    const uint32_t visible = table.visible_column_count();
    for (auto& row : rows) {
        if (row.size() > visible)
            row.erase(row.begin() + static_cast<std::ptrdiff_t>(visible), row.end());
    }
    return Result<std::vector<std::vector<ColumnValue>>>::Ok(std::move(rows));
}

// ------------------ 冷快照（M1-W5，docs/03 §3.2/§3.3）------------------

Result<std::string> Engine::CreateSnapshot() {
    if (!impl_ || !impl_->catalog) return Result<std::string>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr)
        return Result<std::string>::Fail(Err::kReadOnlyHandle);
    if (impl_->image_path.empty()) return Result<std::string>::Fail(Err::kInvalidArgument);

    // 停写语义：根锁内完成 flush/checkpoint 推进/环回收/拷贝（一致性点）。
    // 毒化状态不阻断快照（毒化仅拒写保读，快照不改环、不写行）。
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<std::string>::Fail(guard.error());

    std::string snap_path;
    const Err e = storage::CreateColdSnapshot(impl_->region.get(), impl_->ring.get(),
                                              impl_->image_path, &snap_path);
    if (!IsOk(e)) return Result<std::string>::Fail(e);
    return Result<std::string>::Ok(std::move(snap_path));
}

Err Engine::LockRoot(uint32_t timeout_ms) noexcept {
    auto* m = impl_ ? impl_->region->root_mutex() : nullptr;
    return m != nullptr ? m->Lock(timeout_ms) : Err::kUnsupported;
}

void Engine::UnlockRoot() noexcept {
    if (auto* m = impl_ ? impl_->region->root_mutex() : nullptr) m->Unlock();
}

infra::SlabShmAllocator* Engine::RawAllocator() const noexcept {
    return impl_ ? impl_->region->allocator() : nullptr;
}

infra::ShmRootMutex* Engine::RawRootMutex() const noexcept {
    return impl_ ? impl_->region->root_mutex() : nullptr;
}

// ------------------ TableHandle 成员便利面（M2，docs/04 §3）------------------
// 句柄不拥有引擎：仅转发到发放句柄的 Engine；无效句柄（未绑定/已默认
// 构造）返回 kInvalidArgument。

Result<uint64_t> TableHandle::Upsert(const Row& row) const {
    if (engine_ == nullptr || !IsValid()) return Result<uint64_t>::Fail(Err::kInvalidArgument);
    return engine_->Upsert(*this, row);
}

Result<std::vector<ColumnValue>> TableHandle::Get(const ColumnValue& key) const {
    if (engine_ == nullptr || !IsValid())
        return Result<std::vector<ColumnValue>>::Fail(Err::kInvalidArgument);
    return engine_->GetByKey(*this, key);
}

Result<void> TableHandle::Delete(const ColumnValue& key) const {
    if (engine_ == nullptr || !IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    return engine_->DeleteByKey(*this, key);
}

// ============================================================
// 订阅游标（M3-W2，docs/13 §3-D1）：WalRing 已提交记录 → 增量事件。
// ============================================================

struct ChangeCursor::Impl {
    /// 所属引擎（生命周期须外于游标；Engine 移动不失效——Impl 堆地址不变）。
    Engine::Impl* engine = nullptr;
    /// 过滤器：空 = 全部活跃表；否则 table_id → 表名（订阅时解析快照，
    /// 同名表删除重建后不自动跟随——M3 边界）。
    std::unordered_map<int64_t, std::string> filter;
    /// 已交付到的 seq（游标开区间端点）。
    uint64_t position = 0;
};

namespace {

/// 单次 Next 交付的载荷字节预算（超限提前收批，剩余由后续 Next 交付；
/// 上限与协议载荷上限同量级，docs/13 §3-D5）。
constexpr uint64_t kChangeBatchByteBudget = 1u << 20;  // 1 MiB

/// 估算单条事件的预算占用（含行值容器与字符串的粗估）。
uint64_t EventBudgetBytes(const ChangeEvent& ev) noexcept {
    uint64_t bytes = 128 + ev.table.size();
    for (const ColumnValue& v : ev.row) bytes += 32 + v.bytes_.size();
    return bytes;
}

}  // namespace

ChangeCursor::ChangeCursor() noexcept = default;
ChangeCursor::~ChangeCursor() = default;
ChangeCursor::ChangeCursor(ChangeCursor&&) noexcept = default;
ChangeCursor& ChangeCursor::operator=(ChangeCursor&&) noexcept = default;
ChangeCursor::ChangeCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

bool ChangeCursor::valid() const noexcept { return impl_ != nullptr && impl_->engine != nullptr; }

uint64_t ChangeCursor::Position() const noexcept { return impl_ != nullptr ? impl_->position : 0; }

void ChangeCursor::ResetTo(uint64_t seq) noexcept {
    if (impl_ != nullptr) impl_->position = seq;
}

Result<ChangeCursor> Engine::Subscribe(const std::vector<std::string>& tables) {
    if (impl_ == nullptr) return Result<ChangeCursor>::Fail(Err::kInvalidArgument);
    if (impl_->ring == nullptr) {
        // 未挂载 WAL 环（Reader 主角色）：无订阅数据源。
        return Result<ChangeCursor>::Fail(Err::kUnsupported);
    }

    auto cursor = std::unique_ptr<ChangeCursor::Impl>(new ChangeCursor::Impl());
    cursor->engine = impl_.get();

    // 读路径根锁（M3 TSan 加固）：订阅时解析表名需遍历目录 B+ 树，
    // 与 CreateTable/DropTable 互斥。
    infra::ShmRootMutex* root = impl_->region->root_mutex();
    infra::ShmRootMutexGuard guard(root, kEngineLockTimeoutMs);
    if (root != nullptr && !guard.locked()) return Result<ChangeCursor>::Fail(guard.error());

    const int64_t committed = CommittedSeq();
    cursor->position = committed > 0 ? static_cast<uint64_t>(committed) : 0;

    for (const std::string& name : tables) {
        auto off = impl_->catalog->OpenTable(name);
        if (!off.IsOk()) return Result<ChangeCursor>::Fail(off.Error());
        const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(off.Value());
        if (desc == nullptr ||
            desc->state != static_cast<uint32_t>(storage::TableState::kActive)) {
            return Result<ChangeCursor>::Fail(Err::kTableNotFound);
        }
        cursor->filter.emplace(static_cast<int64_t>(desc->table_id), name);
    }
    return Result<ChangeCursor>::Ok(ChangeCursor(std::move(cursor)));
}

Result<ChangeBatch> ChangeCursor::Next(uint32_t timeout_ms) {
    if (impl_ == nullptr || impl_->engine == nullptr) {
        return Result<ChangeBatch>::Fail(Err::kInvalidArgument);
    }
    Engine::Impl* eng = impl_->engine;
    if (eng->ring == nullptr) return Result<ChangeBatch>::Fail(Err::kUnsupported);

    layout::shm_SuperBlock* sb = eng->region->super_block();
    auto* committed = layout::AsAtomicU64(&sb->committed_seq);
    const auto start = std::chrono::steady_clock::now();

    ChangeBatch batch;
    batch.last_seq = impl_->position;

    for (;;) {
        const uint64_t to = committed->load(std::memory_order_acquire);
        if (to > impl_->position) {
            // ---- 扫描窗口 (position, to]（根锁内：与覆盖丢弃互斥）----
            struct ResolvedTable {
                storage::TableStore* store = nullptr;
                std::string name;  // 目录解析名（全部表模式用）
            };
            std::unordered_map<int64_t, ResolvedTable> cache;
            bool overflow = false;
            uint64_t last = impl_->position;
            {
                infra::ShmRootMutexGuard guard(eng->region->root_mutex(),
                                               kEngineLockTimeoutMs);
                if (!guard.locked()) return Result<ChangeBatch>::Fail(guard.error());
                uint64_t budget = 0;
                const storage::WalRing::ScanRangeFn on_record =
                    [&](const storage::WalRecordView& v) -> bool {
                    if (budget >= kChangeBatchByteBudget) return false;  // 预算收批
                    const char* table_name = nullptr;
                    if (!impl_->filter.empty()) {
                        auto fit = impl_->filter.find(v.table_id);
                        if (fit == impl_->filter.end()) {
                            // 未订阅的表：跳过但推进游标（无缺漏）。
                            budget += 64;
                            return true;
                        }
                        table_name = fit->second.c_str();
                    }
                    auto cit = cache.find(v.table_id);
                    if (cit == cache.end()) {
                        ResolvedTable rt;
                        auto off = eng->catalog->FindById(static_cast<uint64_t>(v.table_id));
                        if (off.IsOk()) {
                            const storage::shm_TableDescriptor* desc =
                                eng->catalog->Descriptor(off.Value());
                            if (desc != nullptr) rt.name = desc->name;
                            auto st = eng->StoreFor(off.Value(), v.table_id);
                            if (st.IsOk()) rt.store = st.Value();
                        }
                        cit = cache.emplace(v.table_id, std::move(rt)).first;
                    }
                    ResolvedTable& rt = cit->second;
                    if (rt.store == nullptr) return true;  // 表已删除/不可附着：跳过
                    if (table_name == nullptr) {
                        if (rt.name.empty()) return true;  // 全部表模式且名不可解析：跳过
                        table_name = rt.name.c_str();
                    }
                    ChangeEvent ev;
                    if (v.op == storage::kWalOpInsert || v.op == storage::kWalOpUpdate) {
                        std::vector<ColumnValue> row =
                            rt.store->DecodeWalRow(v.payload, v.payload_len, v.row_no, v.row_ver);
                        if (row.empty()) return true;  // 畸形记录：防御跳过
                        ev.kind = (v.op == storage::kWalOpInsert)
                                      ? ChangeEvent::Kind::kInsert
                                      : ChangeEvent::Kind::kUpdate;
                        ev.row = std::move(row);
                    } else if (v.op == storage::kWalOpDelete) {
                        ev.kind = ChangeEvent::Kind::kDelete;
                        ev.key = rt.store->DecodeWalKey(v.payload);
                    } else {
                        budget += 64;  // DDL/批标记等：跳过（不交付）
                        return true;
                    }
                    ev.table_id = v.table_id;
                    ev.table = table_name;
                    ev.seq = v.seq;
                    ev.ts_ns = v.ts_ns;
                    ev.row_no = v.row_no;
                    ev.row_ver = v.row_ver;
                    budget += EventBudgetBytes(ev);
                    batch.events.push_back(std::move(ev));
                    return true;
                };
                const Err se = eng->ring->ScanRange(impl_->position, to, on_record, &last);
                if (se == Err::kCursorOverflow) {
                    overflow = true;
                } else if (!IsOk(se)) {
                    return Result<ChangeBatch>::Fail(se);
                }
            }
            if (overflow) {
                // 起点已被回收：调用方须全量重取后 ResetTo。
                return Result<ChangeBatch>::Fail(Err::kCursorOverflow);
            }
            impl_->position = last;
            batch.last_seq = last;
            if (!batch.events.empty()) return Result<ChangeBatch>::Ok(std::move(batch));
            continue;  // 无匹配事件：复查是否有新提交
        }

        // ---- 无新提交：按剩余预算等待或超时收批 ----
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (timeout_ms != kCursorWaitForever &&
            elapsed_ms >= static_cast<long long>(timeout_ms)) {
            return Result<ChangeBatch>::Ok(std::move(batch));
        }
        uint32_t remain = platform::kWaitInfiniteMs;
        if (timeout_ms != kCursorWaitForever) {
            remain = static_cast<uint32_t>(static_cast<long long>(timeout_ms) - elapsed_ms);
        }
        const uint64_t expect = committed->load(std::memory_order_acquire);
        if (expect > impl_->position) continue;  // 等待前已推进：免一次等待
        (void)platform::Wait32(reinterpret_cast<const uint32_t*>(&sb->committed_seq),
                               static_cast<uint32_t>(expect), remain);
        // 唤醒/超时后一律复查真实值（无丢失唤醒语义，docs/13 §3-D4）。
    }
}

// ============================================================
// 元数据与心跳（M3-W3，rtdbd 服务端）
// ============================================================

Result<std::vector<ColumnDef>> Engine::DescribeTable(const TableHandle& table) {
    if (impl_ == nullptr || !table.IsValid())
        return Result<std::vector<ColumnDef>>::Fail(Err::kInvalidArgument);
    // 持根锁读取：与在线加列（ext 链挂接）互斥，列元数据自洽。
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<std::vector<ColumnDef>>::Fail(guard.error());
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(table.desc_off());
    if (desc == nullptr || desc->magic != storage::kTableDescMagic ||
        desc->table_id != static_cast<uint64_t>(table.table_id()) ||
        desc->state != static_cast<uint32_t>(storage::TableState::kActive)) {
        return Result<std::vector<ColumnDef>>::Fail(Err::kTableNotFound);
    }
    const uint32_t visible = table.visible_column_count();
    const uint32_t available = desc->col_count + desc->ext_col_count;
    const uint32_t take = visible < available ? visible : available;
    std::vector<ColInfo> meta;
    const Err ce = CollectColumnInfo(impl_->region->base_address(), desc, take, &meta);
    if (!IsOk(ce)) return Result<std::vector<ColumnDef>>::Fail(ce);

    std::vector<ColumnDef> out;
    out.reserve(take);
    for (uint32_t i = 0; i < take; ++i) {
        ColumnDef cd;
        cd.name = meta[i].name != nullptr ? meta[i].name : "";
        cd.type = meta[i].type;
        // 内联列携带描述符定长覆盖；ext 列恒沿用类型固有宽度（len=0）。
        cd.len = i < desc->col_count ? desc->cols[i].len : 0;
        cd.nullable = meta[i].nullable;
        out.push_back(std::move(cd));
    }
    return Result<std::vector<ColumnDef>>::Ok(std::move(out));
}

void Engine::RefreshWriterHeartbeat() noexcept {
    if (impl_ == nullptr || impl_->region == nullptr) return;
    if (impl_->region->allocator() == nullptr) return;  // Reader：只读映射不写
    auto* hb = layout::AsAtomicU64(&impl_->region->super_block()->writer_heartbeat_ns);
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    hb->store(now_ns, std::memory_order_release);
}

int64_t Engine::WriterHeartbeatNs() const noexcept {
    if (impl_ == nullptr || impl_->region == nullptr) return 0;
    auto* hb = layout::AsAtomicU64(&impl_->region->super_block()->writer_heartbeat_ns);
    return static_cast<int64_t>(hb->load(std::memory_order_acquire));
}

}  // namespace rtdb
