#include "rtdb/engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "infra/failpoint.hpp"
#include "infra/layout.hpp"
#include "infra/shm_mutex.hpp"
#include "infra/shm_region.hpp"
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

}  // namespace

struct Engine::Impl {
    /// 映像文件路径（冷快照拷贝的派生基准；由 Options.file_path 带入）。
    std::string image_path;
    std::unique_ptr<ShmRegion> region;
    std::unique_ptr<storage::Catalog> catalog;
    /// 表行存储句柄缓存（键 = 描述符偏移）；首次访问时按需附着。
    std::unordered_map<uint64_t, std::unique_ptr<storage::TableStore>> stores;
    /// 映像内 WAL 环（写侧角色挂载；Reader 恒为空）。
    std::unique_ptr<storage::WalRing> ring;
    /// 组提交协调器（写侧角色挂载）。
    std::unique_ptr<storage::GroupCommit> commit;

    Impl();
    ~Impl();  // 停后台线程 + 优雅收尾全映像 flush

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// \brief 取（必要时附着）表行存储，并校验句柄与描述符自洽。
    Result<storage::TableStore*> StoreFor(uint64_t desc_off, int64_t expected_table_id) {
        auto it = stores.find(desc_off);
        if (it == stores.end()) {
            auto attach =
                storage::TableStore::Attach(region->base_address(), region->allocator(), desc_off);
            if (!attach.IsOk()) return Result<storage::TableStore*>::Fail(attach.Error());
            it = stores.emplace(desc_off, attach.TakeValue()).first;
        }
        storage::TableStore* store = it->second.get();
        const storage::shm_TableDescriptor* desc = store->descriptor();
        if (desc == nullptr) return Result<storage::TableStore*>::Fail(Err::kCorruption);
        if (desc->table_id != static_cast<uint64_t>(expected_table_id))
            return Result<storage::TableStore*>::Fail(Err::kInvalidArgument);
        if (desc->state != static_cast<uint32_t>(storage::TableState::kActive))
            return Result<storage::TableStore*>::Fail(Err::kTableNotFound);
        return Result<storage::TableStore*>::Ok(store);
    }

    /// \brief 提交标记：推进全局 committed_seq（写侧串行，单调）。
    void MarkCommitted(uint64_t seq) noexcept {
        layout::AsAtomicU64(&region->super_block()->committed_seq)->store(seq);
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
    /// \return kOk / kIoError（flush 失败）/ kLimitExceeded（记录大于整环）
    Err EnsureRingSpace(uint64_t rec_bytes) noexcept {
        if (ring->HasRoomFor(rec_bytes)) return Err::kOk;
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
    Err WalAppendRowOp(storage::TableStore* store, const storage::StagedRowOp& st,
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
    if (opts.instance_name.empty() || opts.file_path.empty())
        return Result<std::unique_ptr<Engine>>::Fail(Err::kInvalidArgument);
    // 头部 + 元数据分区之后至少要有一点可分配空间。
    if (opts.initial_size_bytes < static_cast<uint64_t>(layout::kDataStartOffset) + 4096)
        return Result<std::unique_ptr<Engine>>::Fail(Err::kIncompatibleLayout);

    ShmRegionParams rp;
    rp.map_name = "rtdb_" + opts.instance_name;
    rp.file_path = opts.file_path;
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
    rp.fsync = opts.fsync;

    auto engine = std::unique_ptr<Engine>(new Engine());
    engine->impl_.reset(new Impl());
    engine->impl_->image_path = opts.file_path;
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

        engine->impl_->commit.reset(new storage::GroupCommit(region, sb, opts.fsync));
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
    return impl_ ? impl_->region->super_block()->session_count : -1;
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

    return Result<TableHandle>::Ok(MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                              desc->data_layout_ver, desc->col_count,
                                              desc->ext_col_count, desc->row_cap, desc_off));
}

Result<TableHandle> Engine::OpenTable(std::string_view name) {
    if (!impl_ || !impl_->catalog) return Result<TableHandle>::Fail(Err::kUnsupported);

    Result<uint64_t> desc_res = impl_->catalog->OpenTable(name);
    if (!desc_res.IsOk()) return Result<TableHandle>::Fail(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
    if (desc == nullptr) return Result<TableHandle>::Fail(Err::kCorruption);
    return Result<TableHandle>::Ok(MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                              desc->data_layout_ver, desc->col_count,
                                              desc->ext_col_count, desc->row_cap, desc_off));
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
    // flush 在根锁之外流水线化（组提交聚合，docs/08 §2-D2）。
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

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<std::vector<ColumnValue>>::Fail(store_res.Error());
    auto got = store_res.Value()->Get(key);  // 免锁乐观读（全量当前 schema）
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
                                 const std::vector<ColumnValue>& row) {
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

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<bool>::Fail(store_res.Error());
    return store_res.Value()->Contains(key);
}

// ------------------ 范围扫描（M1-W5，docs/03 §4.2）------------------

Result<std::vector<std::vector<ColumnValue>>> Engine::Scan(const TableHandle& table,
                                                           const KeyRange& range) {
    if (!impl_ || !impl_->catalog) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                               Err::kUnsupported);
    if (!table.IsValid())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(Err::kInvalidArgument);

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<std::vector<std::vector<ColumnValue>>>::Fail(
                                       store_res.Error());

    auto res = store_res.Value()->Scan(range.lo.is_null() ? nullptr : &range.lo,
                                       range.lo_inclusive, range.hi.is_null() ? nullptr : &range.hi,
                                       range.hi_inclusive);
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
        // 注意：BEGIN 落环后若任何后续环节失败，环内将残留未决批——
        // 恢复扫描会把它之后的一切记录并入该批作废，因此必须毒化拒写。
        bool begin_written = false;
        uint8_t cnt_buf[4];
        storage::EncodeU32LE(cnt_buf, static_cast<uint32_t>(ops.size()));
        uint64_t begin_seq = 0;
        phase_err =
            impl_->WalAppendMark(storage::kWalOpBatchBegin, cnt_buf, 4, &begin_seq);
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

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk())
        return Result<std::vector<std::vector<ColumnValue>>>::Fail(store_res.Error());

    // 免锁近似读（同 Scan/GetByKey 的读协议）。
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

}  // namespace rtdb
