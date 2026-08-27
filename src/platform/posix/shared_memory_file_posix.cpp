/// \file POSIX 平台的文件映像共享内存实现（open + ftruncate + mmap）。
/// 数据一致性：同一文件的 mmap(MAP_SHARED) 各视图共享页缓存条目。

#ifndef _WIN32

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "platform/platform.hpp"

namespace rtdb {
namespace platform {

namespace {

Err ErrnoToErr(int e) noexcept {
    switch (e) {
        case ENOENT:
            return Err::kNotFound;
        case EEXIST:
            return Err::kAlreadyExists;
        case ENOSPC:
            return Err::kOutOfMemory;
        default:
            return Err::kIoError;
    }
}

class PosixSharedMemoryFile final : public SharedMemoryFile {
public:
    ~PosixSharedMemoryFile() override { Close(); }

    void* base_address() const noexcept override { return base_; }
    uint64_t size() const noexcept override { return size_; }

    Err flush(const void* addr, size_t len) noexcept override {
        if (msync(addr, len, MS_SYNC) != 0) return ErrnoToErr(errno);
        return Err::kOk;
    }

    static Result<std::unique_ptr<SharedMemoryFile>> Open(const ShmParams& p) {
        int flags = O_CLOEXEC;
        flags |= (p.mode == OpenMode::CreateNew)      ? (O_CREAT | O_EXCL)
                 : (p.mode == OpenMode::OpenOrCreate) ? O_CREAT
                                                      : 0;

        int fd = ::open(p.file_path.c_str(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) return Result<std::unique_ptr<SharedMemoryFile>>::Fail(ErrnoToErr(errno));

        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            const Err err = ErrnoToErr(errno);
            ::close(fd);
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
        }

        const bool readonly_map = p.readonly;
        if (st.st_size < static_cast<off_t>(p.size_bytes)) {
            // 文件比请求小：仅创建/扩容类打开模式允许 ftruncate 补齐；
            // 只读或 OpenExisting 交由上层 SuperBlock 校验给出语义错误。
            if (readonly_map || p.mode == OpenMode::OpenExisting) {
                ::close(fd);
                return Result<std::unique_ptr<SharedMemoryFile>>::Fail(Err::kBadSuperBlock);
            }
            if (::ftruncate(fd, static_cast<off_t>(p.size_bytes)) != 0) {
                const Err err = ErrnoToErr(errno);
                ::close(fd);
                return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
            }
        }

        off_t map_len = st.st_size;
        if (map_len == 0 && !readonly_map) {
            // 全新空文件且与请求一致（ftruncate 已保证），重新取大小。
            if (::fstat(fd, &st) != 0) {
                const Err err = ErrnoToErr(errno);
                ::close(fd);
                return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
            }
            map_len = st.st_size;
        }

        int prot = readonly_map ? PROT_READ : (PROT_READ | PROT_WRITE);
        void* base = ::mmap(nullptr, static_cast<size_t>(map_len), prot, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            const Err err = ErrnoToErr(errno);
            ::close(fd);
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
        }

        auto* obj = new PosixSharedMemoryFile();
        obj->fd_ = fd;
        obj->base_ = base;
        obj->size_ = static_cast<uint64_t>(map_len);
        return Result<std::unique_ptr<SharedMemoryFile>>::Ok(
            std::unique_ptr<SharedMemoryFile>(obj));
    }

private:
    PosixSharedMemoryFile() = default;

    void Close() noexcept {
        if (base_ != nullptr && base_ != MAP_FAILED) ::munmap(base_, size_);
        if (fd_ >= 0) ::close(fd_);
        base_ = nullptr;
        fd_ = -1;
    }

    int fd_ = -1;
    void* base_ = nullptr;
    uint64_t size_ = 0;
};

}  // namespace

Result<std::unique_ptr<SharedMemoryFile>> OpenSharedMemory(const ShmParams& params) {
    return PosixSharedMemoryFile::Open(params);
}

}  // namespace platform
}  // namespace rtdb

#endif  // !_WIN32
