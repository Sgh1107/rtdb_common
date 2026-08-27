#include "rtdb/engine.hpp"

#include <utility>

#include "infra/layout.hpp"
#include "infra/shm_region.hpp"

namespace rtdb {

struct Engine::Impl {
    std::unique_ptr<ShmRegion> region;
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
    return Result<std::unique_ptr<Engine>>::Ok(std::move(engine));
}

int64_t Engine::CommittedSeq() const noexcept {
    return impl_ ? impl_->region->LoadCommittedSeq() : -1;
}

int64_t Engine::SessionCount() const noexcept {
    return impl_ ? impl_->region->super_block()->session_count : -1;
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
