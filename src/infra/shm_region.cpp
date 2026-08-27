#include "infra/shm_region.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>

#include "infra/crc32.hpp"

namespace rtdb {

using namespace layout;         // 本文件主要操作共享内存布局符号
using infra::Crc32;

using layout::shm_SuperBlock;

// ============================================================
// layout 命名空间：初始化与校验
// ============================================================
namespace layout {

namespace {

/// 稳定字段区：从结构体起始到 max_size_bytes 结束（含），运行时字段
/// 与 layout_crc 自身不参与 CRC。
constexpr size_t kStableFieldsBytes =
    offsetof(shm_SuperBlock, max_size_bytes) + sizeof(uint64_t);

}  // namespace

void RefreshLayoutCrc(shm_SuperBlock* sb) noexcept {
  uint8_t snapshot[sizeof(shm_SuperBlock)] = {0};
  std::memcpy(snapshot, sb, kStableFieldsBytes);
  auto* fields = reinterpret_cast<shm_SuperBlock*>(snapshot);
  fields->layout_crc = 0;
  sb->layout_crc = Crc32(snapshot, kStableFieldsBytes);
}

void InitSuperBlock(shm_SuperBlock* sb, uint64_t total_bytes,
                    uint32_t role_flags, uint32_t fsync_policy,
                    int64_t created_unix_ns) {
  std::memset(sb, 0, sizeof(*sb));
  sb->magic = kShmMagic;
  sb->format_version = kShmFormatVersion;
  sb->header_size = sizeof(shm_SuperBlock);
  sb->pointer_width = static_cast<uint32_t>(sizeof(void*));
  sb->created_unix_ns = created_unix_ns;
  sb->max_size_bytes = total_bytes;
  sb->committed_seq = 0;
  sb->writer_heartbeat_ns = 0;
  sb->session_count = 0;
  sb->role_flags = role_flags;
  sb->fsync_policy = fsync_policy;
  RefreshLayoutCrc(sb);
}

Err ValidateSuperBlock(const shm_SuperBlock& sb) noexcept {
  if (sb.magic != kShmMagic) return Err::kBadSuperBlock;
  if (sb.format_version != kShmFormatVersion) return Err::kVersionMismatch;
  if (sb.pointer_width != sizeof(void*)) return Err::kIncompatibleLayout;
  if (sb.header_size != sizeof(shm_SuperBlock)) return Err::kIncompatibleLayout;

  uint8_t snapshot[sizeof(shm_SuperBlock)] = {0};
  std::memcpy(snapshot, &sb, kStableFieldsBytes);
  auto* fields = reinterpret_cast<shm_SuperBlock*>(snapshot);
  const uint32_t expected_crc = fields->layout_crc;
  fields->layout_crc = 0;
  if (Crc32(snapshot, kStableFieldsBytes) != expected_crc)
    return Err::kIncompatibleLayout;
  return Err::kOk;
}

}  // namespace layout

// ============================================================
// ShmRegion
// ============================================================

ShmRegion::~ShmRegion() = default;

Result<std::unique_ptr<ShmRegion>> ShmRegion::Open(
    const ShmRegionParams& p) {
  if (p.file_path.empty() || p.size_bytes < kHeaderReservedBytes)
    return Result<std::unique_ptr<ShmRegion>>::Fail(Err::kInvalidArgument);

  platform::ShmParams pp;
  pp.map_name = p.map_name;
  pp.file_path = p.file_path;
  pp.size_bytes = p.size_bytes;
  pp.mode = p.mode;
  pp.readonly = (p.role == Role::Reader);

  auto file_res = platform::OpenSharedMemory(pp);
  if (!file_res.IsOk())
    return Result<std::unique_ptr<ShmRegion>>::Fail(file_res.Error());

  auto region = std::unique_ptr<ShmRegion>(new ShmRegion());
  region->file_ = file_res.TakeValue();
  region->base_ = region->file_->base_address();
  region->super_block_ = layout::SuperBlockAt(region->base_);

  shm_SuperBlock* sb = region->super_block_;
  // 仅当头部完全为零（真正的新建空文件）才初始化；一旦存在任何
  // 非零内容而 magic 非法，说明是损坏/外来的映像，必须拒绝挂载，
  // 绝不能静默重建覆盖既有数据。
  static const char kZeroHeader[sizeof(shm_SuperBlock)] = {0};
  const bool header_all_zero =
      std::memcmp(sb, kZeroHeader, sizeof(shm_SuperBlock)) == 0;

  if (header_all_zero) {
    int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    layout::InitSuperBlock(sb, p.size_bytes, static_cast<uint32_t>(p.role),
                           static_cast<uint32_t>(p.fsync), now_ns);
    region->Flush();
  } else {
    const Err verr = layout::ValidateSuperBlock(*sb);
    if (!IsOk(verr)) return Result<std::unique_ptr<ShmRegion>>::Fail(verr);
  }

  // 会话计数只允许写侧角色推进（只读映射不可写）。
  if (p.role != Role::Reader) {
    AsAtomicI64(&sb->session_count)->fetch_add(1);
  }

  // 挂载分配器 + 根互斥锁（Reader 只读映射不参与写侧元数据）。
  if (p.role != Role::Reader) {
    auto alloc_res = infra::SlabShmAllocator::Attach(
        region->base_, region->mapped_bytes(), header_all_zero);
    if (!alloc_res.IsOk())
      return Result<std::unique_ptr<ShmRegion>>::Fail(alloc_res.Error());
    region->alloc_ = alloc_res.TakeValue();

    auto* ms = reinterpret_cast<infra::shm_RootMutexState*>(
        static_cast<char*>(region->base_) + kRootMutexOffset);
    if (header_all_zero) infra::ShmRootMutex::InitState(ms);
    region->mutex_.reset(new infra::ShmRootMutex(ms));
  }

  region->Flush();
  return Result<std::unique_ptr<ShmRegion>>::Ok(std::move(region));
}

uint64_t ShmRegion::size_bytes() const noexcept {
  return super_block_->max_size_bytes;
}

int64_t ShmRegion::LoadCommittedSeq() const noexcept {
  return static_cast<int64_t>(
      AsAtomicU64(&super_block_->committed_seq)->load());
}

void ShmRegion::StoreCommittedSeq(int64_t v) noexcept {
  AsAtomicU64(&super_block_->committed_seq)->store(static_cast<uint64_t>(v));
}

Err ShmRegion::Flush() noexcept {
  return file_->flush(base_, kHeaderReservedBytes);
}

}  // namespace rtdb
