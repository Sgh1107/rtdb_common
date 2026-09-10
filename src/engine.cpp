#include "rtdb/engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include "infra/layout.hpp"
#include "infra/shm_mutex.hpp"
#include "infra/shm_region.hpp"
#include "storage/catalog.hpp"
#include "storage/table_store.hpp"

namespace rtdb {

namespace {

/// 引擎内部临界区加锁超时（毫秒）：根锁持有者崩溃时由死亡恢复在
/// 一次活性探测内完成，正常路径远快于此值。
inline constexpr uint32_t kEngineLockTimeoutMs = 5000;

}  // namespace

struct Engine::Impl {
    std::unique_ptr<ShmRegion> region;
    std::unique_ptr<storage::Catalog> catalog;
    /// 表行存储句柄缓存（键 = 描述符偏移）；首次访问时按需附着。
    std::unordered_map<uint64_t, std::unique_ptr<storage::TableStore>> stores;

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
};

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

    return Result<std::unique_ptr<Engine>>::Ok(std::move(engine));
}

int64_t Engine::CommittedSeq() const noexcept {
    return impl_ ? impl_->region->LoadCommittedSeq() : -1;
}

int64_t Engine::SessionCount() const noexcept {
    return impl_ ? impl_->region->super_block()->session_count : -1;
}

TableHandle Engine::MakeHandle(int64_t table_id, const char* name, uint32_t layout_version,
                               uint32_t col_count, uint32_t row_cap, uint64_t desc_off) {
    return TableHandle(table_id, std::string(name != nullptr ? name : ""), layout_version,
                       col_count, row_cap, desc_off);
}

Result<TableHandle> Engine::CreateTable(const TableDef& def) {
    if (!impl_ || !impl_->catalog) return Result<TableHandle>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr)
        return Result<TableHandle>::Fail(Err::kReadOnlyHandle);
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<TableHandle>::Fail(guard.error());

    Result<uint64_t> desc_res = impl_->catalog->CreateTable(def);
    if (!desc_res.IsOk()) return Result<TableHandle>::Fail(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
    if (desc == nullptr) return Result<TableHandle>::Fail(Err::kCorruption);
    return Result<TableHandle>::Ok(MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                              desc->data_layout_ver, desc->col_count, desc->row_cap,
                                              desc_off));
}

Result<TableHandle> Engine::OpenTable(std::string_view name) {
    if (!impl_ || !impl_->catalog) return Result<TableHandle>::Fail(Err::kUnsupported);

    Result<uint64_t> desc_res = impl_->catalog->OpenTable(name);
    if (!desc_res.IsOk()) return Result<TableHandle>::Fail(desc_res.Error());
    const uint64_t desc_off = desc_res.Value();
    const storage::shm_TableDescriptor* desc = impl_->catalog->Descriptor(desc_off);
    if (desc == nullptr) return Result<TableHandle>::Fail(Err::kCorruption);
    return Result<TableHandle>::Ok(MakeHandle(static_cast<int64_t>(desc->table_id), desc->name,
                                              desc->data_layout_ver, desc->col_count, desc->row_cap,
                                              desc_off));
}

Result<void> Engine::DropTable(std::string_view name) {
    if (!impl_ || !impl_->catalog) return Result<void>::Fail(Err::kUnsupported);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);
    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    const Err err = impl_->catalog->DropTable(name);
    if (!IsOk(err)) return Result<void>::Fail(err);
    return Result<void>::Ok();
}

Result<uint64_t> Engine::Insert(const TableHandle& table, const std::vector<ColumnValue>& row) {
    if (!impl_) return Result<uint64_t>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<uint64_t>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<uint64_t>::Fail(Err::kReadOnlyHandle);

    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<uint64_t>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<uint64_t>::Fail(store_res.Error());

    uint64_t row_no = 0;
    const Err err = store_res.Value()->Insert(row, &row_no);
    if (!IsOk(err)) return Result<uint64_t>::Fail(err);
    return Result<uint64_t>::Ok(row_no);
}

Result<std::vector<ColumnValue>> Engine::GetByKey(const TableHandle& table,
                                                  const ColumnValue& key) {
    if (!impl_) return Result<std::vector<ColumnValue>>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<std::vector<ColumnValue>>::Fail(Err::kInvalidArgument);

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<std::vector<ColumnValue>>::Fail(store_res.Error());
    return store_res.Value()->Get(key);  // 免锁乐观读
}

Result<void> Engine::UpdateByKey(const TableHandle& table, const ColumnValue& key,
                                 const std::vector<ColumnValue>& row) {
    if (!impl_) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);

    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());
    const Err err = store_res.Value()->Update(key, row);
    if (!IsOk(err)) return Result<void>::Fail(err);
    return Result<void>::Ok();
}

Result<void> Engine::DeleteByKey(const TableHandle& table, const ColumnValue& key) {
    if (!impl_) return Result<void>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<void>::Fail(Err::kInvalidArgument);
    if (impl_->region->allocator() == nullptr) return Result<void>::Fail(Err::kReadOnlyHandle);

    infra::ShmRootMutexGuard guard(impl_->region->root_mutex(), kEngineLockTimeoutMs);
    if (!guard.locked()) return Result<void>::Fail(guard.error());

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<void>::Fail(store_res.Error());
    const Err err = store_res.Value()->Remove(key);
    if (!IsOk(err)) return Result<void>::Fail(err);
    return Result<void>::Ok();
}

Result<bool> Engine::ContainsKey(const TableHandle& table, const ColumnValue& key) {
    if (!impl_) return Result<bool>::Fail(Err::kUnsupported);
    if (!table.IsValid()) return Result<bool>::Fail(Err::kInvalidArgument);

    auto store_res = impl_->StoreFor(table.desc_off(), table.table_id());
    if (!store_res.IsOk()) return Result<bool>::Fail(store_res.Error());
    return store_res.Value()->Contains(key);
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
