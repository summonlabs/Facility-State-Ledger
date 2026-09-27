// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fsl/status.hpp"

/// \file platform.hpp
/// Internal operating-system boundary: files, durability flushes, atomic
/// replacement and cross-process advisory locks.
///
/// Everything above this header is portable; everything platform-specific is
/// contained here. All handles are opened so that the type of the opened object
/// is checked on the handle rather than on the path, which removes the classic
/// check-then-open race. Handles are always opened share-delete so that a
/// manifest replacement performed by one process cannot be blocked by another
/// process holding the file open for reading.

namespace fsl::detail {

/// Largest single read this layer will perform for a caller.
inline constexpr std::size_t kMaxReadBytes = 512u * 1024u * 1024u;

/// An owned operating-system file handle.
class File {
 public:
  File() noexcept = default;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File();

  /// Opens an existing file for reading.
  [[nodiscard]] static Result<File> open_read(const std::filesystem::path& path);
  /// Opens an existing file for reading and writing.
  [[nodiscard]] static Result<File> open_read_write(const std::filesystem::path& path);
  /// Opens a file for reading and writing, creating it when absent.
  [[nodiscard]] static Result<File> open_read_write_create(const std::filesystem::path& path);
  /// Creates a file exclusively, failing when it already exists.
  [[nodiscard]] static Result<File> create_exclusive(const std::filesystem::path& path);
  /// Creates or truncates a file for writing.
  [[nodiscard]] static Result<File> open_write_truncate(const std::filesystem::path& path);

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;

  /// Writes all bytes at the current end of file.
  [[nodiscard]] Result<void> append(std::span<const std::uint8_t> bytes);
  /// Writes all bytes at an absolute offset, extending the file as needed.
  [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes);
  /// Reads up to `buffer.size()` bytes at an absolute offset; returns how many
  /// were available. A short read means end of file.
  [[nodiscard]] Result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> buffer);

  [[nodiscard]] Result<std::uint64_t> size();
  [[nodiscard]] Result<void> truncate(std::uint64_t size);
  /// Forces written bytes to stable storage. This is the durability boundary.
  [[nodiscard]] Result<void> flush();

  /// Human-readable description of the open handle, for diagnostics only.
  [[nodiscard]] const std::string& description() const noexcept { return description_; }

  /// Adopts an already-open native handle. Used only by this translation unit's
  /// platform helpers; the handle must be exclusive to the returned object.
  [[nodiscard]] static File adopt(void* native_handle, std::string description) noexcept;

 private:
  // On Windows this holds a HANDLE; on POSIX an int file descriptor encoded as
  // (fd + 1) so that 0 keeps meaning "no handle".
  void* handle_ = nullptr;
  std::string description_;
  // Append position for the single-writer model. Resolved from the file size on
  // the first append so that a reopened file continues at its end.
  std::uint64_t append_offset_ = 0;
  bool append_offset_known_ = false;
};

/// How a lock excludes other holders.
enum class LockMode : std::uint8_t {
  /// One holder; excludes both shared and exclusive holders.
  kExclusive = 1,
  /// Many holders; excludes exclusive holders.
  kShared = 2,
};

/// An advisory whole-file lock held for the lifetime of the object.
///
/// The lock is released when the object is destroyed or when the owning process
/// terminates, including on abnormal termination.
class FileLock {
 public:
  FileLock() noexcept = default;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  ~FileLock();

  /// Acquires `mode` on `path`, creating the lock file when absent.
  ///
  /// With `blocking == false` a conflicting holder yields kLocked rather than
  /// waiting.
  [[nodiscard]] static Result<FileLock> acquire(const std::filesystem::path& path,
                                                LockMode mode,
                                                bool blocking);

  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

  /// Adopts an already-locked native handle. Internal to this translation unit.
  [[nodiscard]] static FileLock adopt(void* native_handle) noexcept;

 private:
  void* handle_ = nullptr;
};

/// Creates a directory and any missing parent. Succeeds when it already exists.
[[nodiscard]] Result<void> ensure_directory(const std::filesystem::path& path);
/// Best-effort durability flush of a directory entry. Windows does not expose a
/// reliable equivalent; a failure there is reported as success.
[[nodiscard]] Result<void> flush_directory(const std::filesystem::path& path);
[[nodiscard]] Result<bool> path_exists(const std::filesystem::path& path);
/// True when the path names an existing regular file and not a reparse point.
[[nodiscard]] Result<bool> is_regular_file(const std::filesystem::path& path);
/// Sorted names of the entries of a directory, bounded by `max_entries`.
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::filesystem::path& path,
                                                              std::size_t max_entries);
/// Removes a file. A missing file is reported as success.
[[nodiscard]] Result<void> remove_file(const std::filesystem::path& path);
/// Resolves a path to an absolute, symlink-free form.
[[nodiscard]] Result<std::filesystem::path> absolute_normalized(const std::filesystem::path& path);

/// Atomically replaces `target` with `replacement`.
///
/// When `backup` is supplied and `target` exists, the previous contents of
/// `target` are preserved at `backup` as part of the same operation, so that a
/// reader always sees either the old or the new publication and a recovery pass
/// can fall back to the previous one.
[[nodiscard]] Result<void> atomic_replace(const std::filesystem::path& replacement,
                                          const std::filesystem::path& target,
                                          const std::optional<std::filesystem::path>& backup);

/// Nanoseconds from an unspecified monotonic origin.
[[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept;
/// Nanoseconds since the Unix epoch, or 0 when unavailable.
[[nodiscard]] std::uint64_t unix_nanoseconds() noexcept;

/// Fills a 16-byte buffer from the operating system entropy source.
[[nodiscard]] Result<void> random_bytes(std::span<std::uint8_t> buffer);

}  // namespace fsl::detail
