// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "detail/platform.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

namespace fsl::detail {
namespace {

[[nodiscard]] std::string describe_path(const std::filesystem::path& path) {
  return path.string();
}

}  // namespace

#if defined(_WIN32)

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

[[nodiscard]] Status last_error_status(ErrorCode code, const std::string& what) {
  const DWORD error = ::GetLastError();
  return io_failure(code, what + " (windows error " + std::to_string(static_cast<unsigned long>(error)) + ")");
}

[[nodiscard]] Status path_to_wide(const std::filesystem::path& path, std::wstring& out) {
  out = path.wstring();
  if (out.empty()) {
    return invalid_argument(ErrorCode::kPathInvalid, "empty path");
  }
  if (out.size() > 32000) {
    return invalid_argument(ErrorCode::kPathInvalid, "path exceeds the platform maximum length");
  }
  return Status::ok();
}

[[nodiscard]] Result<File> open_handle(const std::filesystem::path& path,
                                       DWORD access,
                                       DWORD disposition) {
  std::wstring wide;
  if (Status status = path_to_wide(path, wide); status.is_error()) {
    return status;
  }
  // FILE_FLAG_OPEN_REPARSE_POINT keeps a symlink or junction from being followed
  // silently; the opened object is then checked on its handle below.
  const HANDLE handle = ::CreateFileW(wide.c_str(),
                                      access,
                                      kShareAll,
                                      nullptr,
                                      disposition,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    const ErrorCode code = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                               ? ErrorCode::kFileOpenFailed
                               : (error == ERROR_ACCESS_DENIED ? ErrorCode::kPermissionDenied
                                                               : ErrorCode::kFileOpenFailed);
    return io_failure(code, "cannot open " + describe_path(path) + " (windows error " +
                               std::to_string(static_cast<unsigned long>(error)) + ")");
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(handle, &info) == 0) {
    const Status status = last_error_status(ErrorCode::kFileOpenFailed,
                                            "cannot inspect " + describe_path(path));
    ::CloseHandle(handle);
    return status;
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    ::CloseHandle(handle);
    return invalid_argument(ErrorCode::kNotARegularFile,
                            describe_path(path) + " is a reparse point, not a regular file");
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    ::CloseHandle(handle);
    return invalid_argument(ErrorCode::kNotARegularFile,
                            describe_path(path) + " is a directory, not a regular file");
  }

  return File::adopt(handle, describe_path(path));
}

}  // namespace

File File::adopt(void* native_handle, std::string description) noexcept {
  File file;
  file.handle_ = native_handle;
  file.description_ = std::move(description);
  return file;
}

File::File(File&& other) noexcept
    : handle_(other.handle_),
      description_(std::move(other.description_)),
      append_offset_(other.append_offset_),
      append_offset_known_(other.append_offset_known_) {
  other.handle_ = nullptr;
  other.append_offset_known_ = false;
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    description_ = std::move(other.description_);
    append_offset_ = other.append_offset_;
    append_offset_known_ = other.append_offset_known_;
    other.handle_ = nullptr;
    other.append_offset_known_ = false;
  }
  return *this;
}

File::~File() { close(); }

bool File::valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

void File::close() noexcept {
  if (valid()) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
  handle_ = nullptr;
  append_offset_known_ = false;
}

Result<File> File::open_read(const std::filesystem::path& path) {
  return open_handle(path, GENERIC_READ, OPEN_EXISTING);
}

Result<File> File::open_read_write(const std::filesystem::path& path) {
  return open_handle(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
}

Result<File> File::open_read_write_create(const std::filesystem::path& path) {
  return open_handle(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
}

Result<File> File::create_exclusive(const std::filesystem::path& path) {
  return open_handle(path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW);
}

Result<File> File::open_write_truncate(const std::filesystem::path& path) {
  return open_handle(path, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
}

Result<void> File::append(std::span<const std::uint8_t> bytes) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileWriteFailed, "write on an invalid handle");
  }
  if (bytes.empty()) {
    return Status::ok();
  }
  if (!append_offset_known_) {
    auto measured = size();
    if (!measured.has_value()) {
      return measured.status();
    }
    append_offset_ = measured.value();
    append_offset_known_ = true;
  }
  // write_at already advances the tracked append position; capture the offset
  // first so that the two bookkeeping paths cannot diverge.
  const std::uint64_t offset = append_offset_;
  Status status = write_at(offset, bytes);
  if (status.is_ok()) {
    append_offset_ = offset + bytes.size();
  }
  return status;
}

Result<void> File::write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileWriteFailed, "write on an invalid handle");
  }
  if (bytes.empty()) {
    return Status::ok();
  }
  const std::uint64_t end = offset + bytes.size();
  if (append_offset_known_ && end > append_offset_) {
    append_offset_ = end;
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  std::size_t written = 0;
  while (written < bytes.size()) {
    DWORD chunk = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 30));
    OVERLAPPED current = overlapped;
    const std::uint64_t position = offset + written;
    current.Offset = static_cast<DWORD>(position & 0xFFFFFFFFu);
    current.OffsetHigh = static_cast<DWORD>(position >> 32);
    const BOOL ok = ::WriteFile(static_cast<HANDLE>(handle_),
                                bytes.data() + written,
                                request,
                                &chunk,
                                &current);
    if (ok == 0 || chunk == 0) {
      return last_error_status(ErrorCode::kFileWriteFailed, "cannot write " + description_);
    }
    written += chunk;
  }
  return Status::ok();
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<std::uint8_t> buffer) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileReadFailed, "read on an invalid handle");
  }
  if (buffer.empty()) {
    return static_cast<std::size_t>(0);
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD read = 0;
  if (::ReadFile(static_cast<HANDLE>(handle_), buffer.data(),
                 static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1u << 30)), &read, &overlapped) == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_HANDLE_EOF) {
      return static_cast<std::size_t>(0);
    }
    return last_error_status(ErrorCode::kFileReadFailed, "cannot read " + description_);
  }
  return static_cast<std::size_t>(read);
}

Result<std::uint64_t> File::size() {
  if (!valid()) {
    return io_failure(ErrorCode::kFileReadFailed, "size of an invalid handle");
  }
  LARGE_INTEGER value{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &value) == 0) {
    return last_error_status(ErrorCode::kFileReadFailed, "cannot measure " + description_);
  }
  return static_cast<std::uint64_t>(value.QuadPart);
}

Result<void> File::truncate(std::uint64_t size) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileTruncateFailed, "truncate on an invalid handle");
  }
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFilePointerEx(static_cast<HANDLE>(handle_), distance, nullptr, FILE_BEGIN) == 0) {
    return last_error_status(ErrorCode::kFileTruncateFailed, "cannot seek " + description_);
  }
  if (::SetEndOfFile(static_cast<HANDLE>(handle_)) == 0) {
    return last_error_status(ErrorCode::kFileTruncateFailed, "cannot truncate " + description_);
  }
  append_offset_ = size;
  append_offset_known_ = true;
  return Status::ok();
}

Result<void> File::flush() {
  if (!valid()) {
    return io_failure(ErrorCode::kFileFlushFailed, "flush on an invalid handle");
  }
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return last_error_status(ErrorCode::kFileFlushFailed, "cannot flush " + description_);
  }
  return Status::ok();
}

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::held() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

void FileLock::release() noexcept {
  if (held()) {
    OVERLAPPED overlapped{};
    ::UnlockFileEx(static_cast<HANDLE>(handle_), 0, MAXDWORD, MAXDWORD, &overlapped);
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
  handle_ = nullptr;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, LockMode mode, bool blocking) {
  std::wstring wide;
  if (Status status = path_to_wide(path, wide); status.is_error()) {
    return status;
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      kShareAll,
                                      nullptr,
                                      OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status(ErrorCode::kFileLockFailed, "cannot open lock file " + describe_path(path));
  }

  OVERLAPPED overlapped{};
  DWORD flags = 0;
  if (mode == LockMode::kExclusive) {
    flags |= LOCKFILE_EXCLUSIVE_LOCK;
  }
  if (!blocking) {
    flags |= LOCKFILE_FAIL_IMMEDIATELY;
  }
  if (::LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    if (!blocking && (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING)) {
      return locked(ErrorCode::kLedgerLocked,
                    "another process holds the writer lock for " + describe_path(path.parent_path()));
    }
    return io_failure(ErrorCode::kFileLockFailed,
                      "cannot lock " + describe_path(path) + " (windows error " +
                          std::to_string(static_cast<unsigned long>(error)) + ")");
  }

  FileLock result;
  result.handle_ = handle;
  return result;
}

FileLock FileLock::adopt(void* native_handle) noexcept {
  FileLock lock;
  lock.handle_ = native_handle;
  return lock;
}

Result<void> ensure_directory(const std::filesystem::path& path) {
  std::wstring wide;
  if (Status status = path_to_wide(path, wide); status.is_error()) {
    return status;
  }
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Status::ok();
  }
  if (::CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
      std::error_code recheck;
      if (std::filesystem::is_directory(path, recheck)) {
        return Status::ok();
      }
      return invalid_argument(ErrorCode::kNotARegularFile,
                              describe_path(path) + " exists and is not a directory");
    }
    return io_failure(ErrorCode::kDirectoryOperationFailed,
                      "cannot create " + describe_path(path) + " (windows error " +
                          std::to_string(static_cast<unsigned long>(code)) + ")");
  }
  return Status::ok();
}

Result<void> flush_directory(const std::filesystem::path& path) {
  // Windows exposes no dependable directory fsync. The manifest replacement uses
  // MOVEFILE_WRITE_THROUGH, which is the documented durability primitive here.
  std::error_code error;
  if (!std::filesystem::is_directory(path, error)) {
    return io_failure(ErrorCode::kDirectoryOperationFailed,
                      describe_path(path) + " is not a directory");
  }
  return Status::ok();
}

Result<bool> path_exists(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error && !exists) {
    return io_failure(ErrorCode::kFileOpenFailed,
                      "cannot test " + describe_path(path) + ": " + error.message());
  }
  return exists;
}

Result<bool> is_regular_file(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return false;
    }
    return io_failure(ErrorCode::kFileOpenFailed,
                      "cannot inspect " + describe_path(path) + ": " + error.message());
  }
  if (std::filesystem::is_symlink(status)) {
    return false;
  }
  return std::filesystem::is_regular_file(status);
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& path,
                                                std::size_t max_entries) {
  std::error_code error;
  std::vector<std::string> names;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return io_failure(ErrorCode::kDirectoryOperationFailed,
                      "cannot list " + describe_path(path) + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    if (names.size() >= max_entries) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "directory " + describe_path(path) + " holds more than " +
                                   std::to_string(max_entries) + " entries");
    }
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  std::wstring wide;
  if (Status status = path_to_wide(path, wide); status.is_error()) {
    return status;
  }
  if (::DeleteFileW(wide.c_str()) != 0) {
    return Status::ok();
  }
  const DWORD code = ::GetLastError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return Status::ok();
  }
  return io_failure(ErrorCode::kFileRenameFailed,
                    "cannot remove " + describe_path(path) + " (windows error " +
                        std::to_string(static_cast<unsigned long>(code)) + ")");
}

Result<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    return invalid_argument(ErrorCode::kPathInvalid,
                            "cannot resolve " + describe_path(path) + ": " + error.message());
  }
  std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
  if (error) {
    return invalid_argument(ErrorCode::kPathInvalid,
                            "cannot canonicalise " + describe_path(path) + ": " + error.message());
  }
  return canonical;
}

Result<void> atomic_replace(const std::filesystem::path& replacement,
                            const std::filesystem::path& target,
                            const std::optional<std::filesystem::path>& backup) {
  std::wstring wide_target;
  std::wstring wide_replacement;
  if (Status status = path_to_wide(target, wide_target); status.is_error()) {
    return status;
  }
  if (Status status = path_to_wide(replacement, wide_replacement); status.is_error()) {
    return status;
  }

  std::error_code error;
  const bool target_exists = std::filesystem::exists(target, error) && !error;
  if (!target_exists) {
    if (::MoveFileExW(wide_replacement.c_str(), wide_target.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
      return last_error_status(ErrorCode::kFileRenameFailed,
                               "cannot publish " + describe_path(target));
    }
    return Status::ok();
  }

  if (backup.has_value()) {
    std::wstring wide_backup;
    if (Status status = path_to_wide(*backup, wide_backup); status.is_error()) {
      return status;
    }
    if (::ReplaceFileW(wide_target.c_str(),
                       wide_replacement.c_str(),
                       wide_backup.c_str(),
                       REPLACEFILE_WRITE_THROUGH,
                       nullptr,
                       nullptr) != 0) {
      return Status::ok();
    }
    const DWORD code = ::GetLastError();
    if (code != ERROR_UNABLE_TO_MOVE_REPLACEMENT && code != ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 &&
        code != ERROR_FILE_NOT_FOUND) {
      return io_failure(ErrorCode::kFileRenameFailed,
                        "cannot replace " + describe_path(target) + " (windows error " +
                            std::to_string(static_cast<unsigned long>(code)) + ")");
    }
    // ReplaceFile cannot create a backup when the target has none; fall through
    // to a plain replace.
  }

  if (::MoveFileExW(wide_replacement.c_str(), wide_target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return last_error_status(ErrorCode::kFileRenameFailed, "cannot publish " + describe_path(target));
  }
  return Status::ok();
}

std::uint64_t monotonic_nanoseconds() noexcept {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  if (::QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart <= 0) {
    return 0;
  }
  if (::QueryPerformanceCounter(&counter) == 0) {
    return 0;
  }
  const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
  const auto per_second = static_cast<std::uint64_t>(frequency.QuadPart);
  const std::uint64_t seconds = ticks / per_second;
  const std::uint64_t remainder = ticks % per_second;
  return seconds * 1000000000ULL + (remainder * 1000000000ULL) / per_second;
}

std::uint64_t unix_nanoseconds() noexcept {
  FILETIME file_time{};
  ::GetSystemTimeAsFileTime(&file_time);
  ULARGE_INTEGER value{};
  value.LowPart = file_time.dwLowDateTime;
  value.HighPart = file_time.dwHighDateTime;
  // FILETIME counts 100 ns intervals since 1601-01-01.
  constexpr std::uint64_t kUnixEpochInFileTime = 116444736000000000ULL;
  if (value.QuadPart < kUnixEpochInFileTime) {
    return 0;
  }
  return (static_cast<std::uint64_t>(value.QuadPart) - kUnixEpochInFileTime) * 100ULL;
}

Result<void> random_bytes(std::span<std::uint8_t> buffer) {
  if (buffer.empty()) {
    return Status::ok();
  }
  // BCryptGenRandom is resolved dynamically so that the library keeps working on
  // systems where the provider is unavailable.
  using GenRandomFn = long(WINAPI*)(void*, unsigned char*, unsigned long, unsigned long);
  static const GenRandomFn generator = []() -> GenRandomFn {
    const HMODULE module = ::LoadLibraryW(L"bcrypt.dll");
    if (module == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<GenRandomFn>(reinterpret_cast<void*>(
        ::GetProcAddress(module, "BCryptGenRandom")));
  }();
  constexpr unsigned long kUseSystemPreferredRng = 0x00000002u;
  if (generator != nullptr) {
    const long status = generator(nullptr,
                                  buffer.data(),
                                  static_cast<unsigned long>(buffer.size()),
                                  kUseSystemPreferredRng);
    if (status >= 0) {
      return Status::ok();
    }
  }
  return io_failure(ErrorCode::kInternalError, "the operating system entropy source is unavailable");
}

#else  // POSIX

namespace {

[[nodiscard]] Status errno_status(ErrorCode code, const std::string& what) {
  return io_failure(code, what + " (errno " + std::to_string(errno) + ": " + std::strerror(errno) + ")");
}

[[nodiscard]] int to_fd(void* handle) { return static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1); }

[[nodiscard]] void* from_fd(int fd) { return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd + 1)); }

[[nodiscard]] Result<File> open_fd(const std::filesystem::path& path, int flags, mode_t mode) {
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
  if (fd < 0) {
    const ErrorCode code = (errno == EACCES || errno == EPERM) ? ErrorCode::kPermissionDenied
                                                               : ErrorCode::kFileOpenFailed;
    return errno_status(code, "cannot open " + describe_path(path));
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const Status status = errno_status(ErrorCode::kFileOpenFailed, "cannot inspect " + describe_path(path));
    ::close(fd);
    return status;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    return invalid_argument(ErrorCode::kNotARegularFile,
                            describe_path(path) + " is not a regular file");
  }
  return File::adopt(from_fd(fd), describe_path(path));
}

}  // namespace

File File::adopt(void* native_handle, std::string description) noexcept {
  File file;
  file.handle_ = native_handle;
  file.description_ = std::move(description);
  return file;
}

File::File(File&& other) noexcept
    : handle_(other.handle_),
      description_(std::move(other.description_)),
      append_offset_(other.append_offset_),
      append_offset_known_(other.append_offset_known_) {
  other.handle_ = nullptr;
  other.append_offset_known_ = false;
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    description_ = std::move(other.description_);
    append_offset_ = other.append_offset_;
    append_offset_known_ = other.append_offset_known_;
    other.handle_ = nullptr;
    other.append_offset_known_ = false;
  }
  return *this;
}

File::~File() { close(); }

bool File::valid() const noexcept { return handle_ != nullptr; }

void File::close() noexcept {
  if (valid()) {
    ::close(to_fd(handle_));
  }
  handle_ = nullptr;
  append_offset_known_ = false;
}

Result<File> File::open_read(const std::filesystem::path& path) {
  return open_fd(path, O_RDONLY, 0);
}

Result<File> File::open_read_write(const std::filesystem::path& path) {
  return open_fd(path, O_RDWR, 0);
}

Result<File> File::open_read_write_create(const std::filesystem::path& path) {
  return open_fd(path, O_RDWR | O_CREAT, 0644);
}

Result<File> File::create_exclusive(const std::filesystem::path& path) {
  return open_fd(path, O_RDWR | O_CREAT | O_EXCL, 0644);
}

Result<File> File::open_write_truncate(const std::filesystem::path& path) {
  return open_fd(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}

Result<void> File::append(std::span<const std::uint8_t> bytes) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileWriteFailed, "write on an invalid handle");
  }
  if (bytes.empty()) {
    return Status::ok();
  }
  if (!append_offset_known_) {
    auto measured = size();
    if (!measured.has_value()) {
      return measured.status();
    }
    append_offset_ = measured.value();
    append_offset_known_ = true;
  }
  // write_at already advances the tracked append position; capture the offset
  // first so that the two bookkeeping paths cannot diverge.
  const std::uint64_t offset = append_offset_;
  Status status = write_at(offset, bytes);
  if (status.is_ok()) {
    append_offset_ = offset + bytes.size();
  }
  return status;
}

Result<void> File::write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileWriteFailed, "write on an invalid handle");
  }
  if (bytes.empty()) {
    return Status::ok();
  }
  const std::uint64_t end = offset + bytes.size();
  if (append_offset_known_ && end > append_offset_) {
    append_offset_ = end;
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t chunk = ::pwrite(to_fd(handle_),
                                   bytes.data() + written,
                                   bytes.size() - written,
                                   static_cast<off_t>(offset + written));
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(ErrorCode::kFileWriteFailed, "cannot write " + description_);
    }
    written += static_cast<std::size_t>(chunk);
  }
  return Status::ok();
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<std::uint8_t> buffer) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileReadFailed, "read on an invalid handle");
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t chunk = ::pread(to_fd(handle_),
                                  buffer.data() + total,
                                  buffer.size() - total,
                                  static_cast<off_t>(offset + total));
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(ErrorCode::kFileReadFailed, "cannot read " + description_);
    }
    if (chunk == 0) {
      break;
    }
    total += static_cast<std::size_t>(chunk);
  }
  return total;
}

Result<std::uint64_t> File::size() {
  if (!valid()) {
    return io_failure(ErrorCode::kFileReadFailed, "size of an invalid handle");
  }
  struct stat info {};
  if (::fstat(to_fd(handle_), &info) != 0) {
    return errno_status(ErrorCode::kFileReadFailed, "cannot measure " + description_);
  }
  return static_cast<std::uint64_t>(info.st_size);
}

Result<void> File::truncate(std::uint64_t size) {
  if (!valid()) {
    return io_failure(ErrorCode::kFileTruncateFailed, "truncate on an invalid handle");
  }
  if (::ftruncate(to_fd(handle_), static_cast<off_t>(size)) != 0) {
    return errno_status(ErrorCode::kFileTruncateFailed, "cannot truncate " + description_);
  }
  append_offset_ = size;
  append_offset_known_ = true;
  return Status::ok();
}

Result<void> File::flush() {
  if (!valid()) {
    return io_failure(ErrorCode::kFileFlushFailed, "flush on an invalid handle");
  }
  if (::fsync(to_fd(handle_)) != 0) {
    return errno_status(ErrorCode::kFileFlushFailed, "cannot flush " + description_);
  }
  return Status::ok();
}

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  if (held()) {
    ::flock(to_fd(handle_), LOCK_UN);
    ::close(to_fd(handle_));
  }
  handle_ = nullptr;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, LockMode mode, bool blocking) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return errno_status(ErrorCode::kFileLockFailed, "cannot open lock file " + describe_path(path));
  }
  int operation = (mode == LockMode::kExclusive) ? LOCK_EX : LOCK_SH;
  if (!blocking) {
    operation |= LOCK_NB;
  }
  while (::flock(fd, operation) != 0) {
    if (errno == EINTR) {
      continue;
    }
    const bool would_block = (errno == EWOULDBLOCK);
    ::close(fd);
    if (!blocking && would_block) {
      return locked(ErrorCode::kLedgerLocked,
                    "another process holds the writer lock for " + describe_path(path.parent_path()));
    }
    return io_failure(ErrorCode::kFileLockFailed, "cannot lock " + describe_path(path));
  }
  FileLock result;
  result.handle_ = from_fd(fd);
  return result;
}

FileLock FileLock::adopt(void* native_handle) noexcept {
  FileLock lock;
  lock.handle_ = native_handle;
  return lock;
}

Result<void> ensure_directory(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Status::ok();
  }
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return errno_status(ErrorCode::kDirectoryOperationFailed, "cannot create " + describe_path(path));
  }
  return Status::ok();
}

Result<void> flush_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return errno_status(ErrorCode::kDirectoryOperationFailed, "cannot open " + describe_path(path));
  }
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) {
    return errno_status(ErrorCode::kDirectoryOperationFailed, "cannot flush " + describe_path(path));
  }
  return Status::ok();
}

Result<bool> path_exists(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error && !exists) {
    return io_failure(ErrorCode::kFileOpenFailed,
                      "cannot test " + describe_path(path) + ": " + error.message());
  }
  return exists;
}

Result<bool> is_regular_file(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return false;
    }
    return io_failure(ErrorCode::kFileOpenFailed,
                      "cannot inspect " + describe_path(path) + ": " + error.message());
  }
  if (std::filesystem::is_symlink(status)) {
    return false;
  }
  return std::filesystem::is_regular_file(status);
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& path,
                                                std::size_t max_entries) {
  std::error_code error;
  std::vector<std::string> names;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return io_failure(ErrorCode::kDirectoryOperationFailed,
                      "cannot list " + describe_path(path) + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    if (names.size() >= max_entries) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "directory " + describe_path(path) + " holds more than " +
                                   std::to_string(max_entries) + " entries");
    }
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return errno_status(ErrorCode::kFileRenameFailed, "cannot remove " + describe_path(path));
  }
  return Status::ok();
}

Result<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    return invalid_argument(ErrorCode::kPathInvalid,
                            "cannot resolve " + describe_path(path) + ": " + error.message());
  }
  std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
  if (error) {
    return invalid_argument(ErrorCode::kPathInvalid,
                            "cannot canonicalise " + describe_path(path) + ": " + error.message());
  }
  return canonical;
}

Result<void> atomic_replace(const std::filesystem::path& replacement,
                            const std::filesystem::path& target,
                            const std::optional<std::filesystem::path>& backup) {
  if (backup.has_value()) {
    std::error_code error;
    if (std::filesystem::exists(target, error) && !error) {
      std::filesystem::path staging = *backup;
      staging += ".tmp";
      std::error_code copy_error;
      std::filesystem::copy_file(target, staging, std::filesystem::copy_options::overwrite_existing,
                                 copy_error);
      if (copy_error) {
        return io_failure(ErrorCode::kFileRenameFailed,
                          "cannot stage backup " + describe_path(staging) + ": " + copy_error.message());
      }
      if (::rename(staging.c_str(), backup->c_str()) != 0) {
        return errno_status(ErrorCode::kFileRenameFailed, "cannot publish " + describe_path(*backup));
      }
    }
  }
  if (::rename(replacement.c_str(), target.c_str()) != 0) {
    return errno_status(ErrorCode::kFileRenameFailed, "cannot publish " + describe_path(target));
  }
  return Status::ok();
}

std::uint64_t monotonic_nanoseconds() noexcept {
  struct timespec value {};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t unix_nanoseconds() noexcept {
  struct timespec value {};
  if (::clock_gettime(CLOCK_REALTIME, &value) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

Result<void> random_bytes(std::span<std::uint8_t> buffer) {
  if (buffer.empty()) {
    return Status::ok();
  }
  File source;
  auto opened = File::open_read("/dev/urandom");
  if (!opened.has_value()) {
    return opened.status();
  }
  source = std::move(opened).value();
  std::size_t filled = 0;
  while (filled < buffer.size()) {
    auto chunk = source.read_at(filled, buffer.subspan(filled));
    if (!chunk.has_value()) {
      return chunk.status();
    }
    if (chunk.value() == 0) {
      return io_failure(ErrorCode::kInternalError, "the system entropy source returned end of file");
    }
    filled += chunk.value();
  }
  return Status::ok();
}

#endif

}  // namespace fsl::detail
