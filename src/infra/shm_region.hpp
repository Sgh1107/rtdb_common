#ifndef RTDB_SRC_INFRA_SHM_REGION_HPP_
#define RTDB_SRC_INFRA_SHM_REGION_HPP_

#include <memory>

#include "platform/platform.hpp"
#include "infra/slab_allocator.hpp"
#include "infra/shm_mutex.hpp"
#include "rtdb/api_macro.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"
#include "infra/layout.hpp"

namespace rtdb {

struct ShmRegionParams {
    std::string map_name;
    std::string file_path;
    uint64_t size_bytes;  ///< 含头部的总容量
    OpenMode mode = OpenMode::OpenOrCreate;
    Role role = Role::Standalone;
    FsyncPolicy fsync = FsyncPolicy::PerSecond;
};

/// 共享内存区的宿主侧包装：负责打开映射 + SuperBlock 校验/初始化 +
/// 全局状态字的类型安全访问（docs/03 §2.1 ShmRegion）。
///
/// 注：本类属内部 infra 组件，但单元测试需白盒直建区域（docs/06 §1
/// "组件测试落真实 tmpdir 映像"），故随 rtdbcore 导出符号（与下方
/// layout 自由函数同一处理），公共 API 面仍以 include/rtdb/ 为准。
///
/// C4251（unique_ptr 成员需 dll 接口）对本类为误报：客户端仅经 Open 的
/// 返回值持有本对象，从不直接销毁 file_/alloc_/mutex_ 成员，且成员类型
/// 的完整定义随本头一并可见。定向抑制，避免淹没 /W4 下的真实告警。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class RTDB_API ShmRegion {
public:
    ~ShmRegion();

    ShmRegion(const ShmRegion&) = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;

    /// 打开或创建并完成校验。新建路径会写入完整 SuperBlock（含布局
    /// CRC）；既有路径依次校验 magic → 版本 → 字宽 → CRC，分别返回
    /// kBadSuperBlock / kVersionMismatch / kIncompatibleLayout。
    /// 打开成功会将 session_count 原子 +1。
    static Result<std::unique_ptr<ShmRegion>> Open(const ShmRegionParams& p);

    layout::shm_SuperBlock* super_block() const noexcept { return super_block_; }
    void* base_address() const noexcept { return base_; }
    uint64_t size_bytes() const noexcept;

    /// 实际映射字节数（可能大于请求值：Windows 对齐/历史扩容遗留）。
    uint64_t mapped_bytes() const noexcept { return file_->size(); }

    /// 仅非 Reader 角色挂载。生命周期与本区一致，宿主进程内复用。
    infra::SlabShmAllocator* allocator() const noexcept { return alloc_.get(); }
    infra::ShmRootMutex* root_mutex() const noexcept { return mutex_.get(); }

    int64_t LoadCommittedSeq() const noexcept;
    void StoreCommittedSeq(int64_t v) noexcept;
    Err Flush() noexcept;   ///< 头部区间回写文件
    Err FlushAll() noexcept;  ///< 全映像回写文件（组提交/覆盖协议/优雅收尾）

private:
    ShmRegion() = default;

    std::unique_ptr<platform::SharedMemoryFile> file_;
    void* base_ = nullptr;
    layout::shm_SuperBlock* super_block_ = nullptr;
    std::unique_ptr<infra::SlabShmAllocator> alloc_;
    std::unique_ptr<infra::ShmRootMutex> mutex_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace layout {
/// 填充 SuperBlock 初始内容（调用前区域已清零）。
RTDB_API void InitSuperBlock(shm_SuperBlock* sb, uint64_t total_bytes, uint32_t role_flags,
                             uint32_t fsync_policy, int64_t created_unix_ns);

/// 重新计算稳定字段区 CRC 写入 layout_crc。
RTDB_API void RefreshLayoutCrc(shm_SuperBlock* sb) noexcept;

/// 依序校验，返回首个失败的错误码。
RTDB_API Err ValidateSuperBlock(const shm_SuperBlock& sb) noexcept;
}  // namespace layout

}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SHM_REGION_HPP_
