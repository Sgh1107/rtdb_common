/// \file Windows 平台的文件映像共享内存实现。
/// 依赖 CreateFileW + CreateFileMappingW + MapViewOfFile。
/// 数据一致性：同一文件的两个映射视图共享物理页，跨进程可见。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform/platform.hpp"
#include "rtdb/version.hpp"

namespace rtdb {
namespace platform {

namespace {

std::wstring Utf8ToWide(const std::string& s) noexcept {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(),
                                   static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), static_cast<int>(s.size()),
                        &wide[0], need);
    return wide;
}

Err LastErrorToErr(DWORD e) noexcept {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Err::kNotFound;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return Err::kAlreadyExists;
        case ERROR_DISK_FULL:
            return Err::kOutOfMemory;
        default:
            return Err::kIoError;
    }
}

class WinSharedMemoryFile final : public SharedMemoryFile {
public:
    ~WinSharedMemoryFile() override { Close(); }

    void* base_address() const noexcept override { return base_; }
    uint64_t size() const noexcept override { return size_; }

    Err flush(const void* addr, size_t len) noexcept override {
        if (!base_) return Err::kInternal;
        if (!FlushViewOfFile(addr, len)) return LastErrorToErr(GetLastError());
        return Err::kOk;
    }

    static Result<std::unique_ptr<SharedMemoryFile>> Open(const ShmParams& p) {
        DWORD desired_access = p.readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DWORD creation = OPEN_EXISTING;
        switch (p.mode) {
            case OpenMode::OpenOrCreate:
                creation = OPEN_ALWAYS;  // 存在则打开，不存在则创建空文件
                break;
            case OpenMode::CreateNew:
                creation = CREATE_NEW;
                break;
            case OpenMode::OpenExisting:
                creation = OPEN_EXISTING;
                break;
        }

        const std::wstring wpath = Utf8ToWide(p.file_path);
        HANDLE file = CreateFileW(wpath.c_str(), desired_access, share_mode, nullptr, creation,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(LastErrorToErr(GetLastError()));

        // CreateFileMapping 会自动把零长度文件扩展到 dwMaximumSizeLow，
        // 因此无需手动 SetFilePointer/SetEndOfFile。
        HANDLE mapping = CreateFileMappingW(
            file, nullptr, p.readonly ? PAGE_READONLY : PAGE_READWRITE,
            static_cast<DWORD>(p.size_bytes >> 32), static_cast<DWORD>(p.size_bytes & 0xFFFFFFFFu),
            nullptr);  // 匿名命名对象：靠文件句柄共享即可
        if (mapping == nullptr) {
            const Err err = LastErrorToErr(GetLastError());
            CloseHandle(file);
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
        }

        DWORD view_access = p.readonly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
        void* base = MapViewOfFile(mapping, view_access, 0, 0, 0);
        if (base == nullptr) {
            const Err err = LastErrorToErr(GetLastError());
            CloseHandle(mapping);
            CloseHandle(file);
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(err);
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base, &mbi, sizeof(mbi))) {
            UnmapViewOfFile(base);
            CloseHandle(mapping);
            CloseHandle(file);
            return Result<std::unique_ptr<SharedMemoryFile>>::Fail(Err::kIoError);
        }

        auto obj = std::unique_ptr<WinSharedMemoryFile>(new WinSharedMemoryFile());
        obj->file_ = file;
        obj->mapping_ = mapping;
        obj->base_ = base;
        obj->size_ = static_cast<uint64_t>(mbi.RegionSize);
        return Result<std::unique_ptr<SharedMemoryFile>>::Ok(
            std::unique_ptr<SharedMemoryFile>(obj.release()));
    }

private:
    WinSharedMemoryFile() = default;

    void Close() noexcept {
        if (base_ != nullptr) UnmapViewOfFile(base_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        base_ = nullptr;
        mapping_ = nullptr;
        file_ = nullptr;
    }

    HANDLE file_ = nullptr;  ///< INVALID_HANDLE_VALUE 视为空
    HANDLE mapping_ = nullptr;
    void* base_ = nullptr;
    uint64_t size_ = 0;
};

}  // namespace

Result<std::unique_ptr<SharedMemoryFile>> OpenSharedMemory(const ShmParams& params) {
    return WinSharedMemoryFile::Open(params);
}

}  // namespace platform
}  // namespace rtdb
