// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fsl/clock.hpp"
#include "fsl/fault.hpp"
#include "fsl/status.hpp"

/// \file options.hpp
/// Opening modes, admission bounds and recovery policy.
///
/// Every bound in LedgerOptions is enforced before the corresponding allocation
/// or traversal, so an adversarial ledger directory or an adversarial caller
/// cannot make the library consume unbounded memory or time.

namespace fsl {

/// How a handle intends to use the ledger directory.
enum class OpenMode : std::uint8_t {
  /// Exclusive writer: takes the directory's writer lock, may append and rotate.
  kReadWrite = 1,
  /// Shared reader: never mutates, never takes the writer lock.
  kReadOnly = 2,
  /// Creates the directory if it does not exist, then behaves as kReadWrite.
  kCreate = 3,
  /// Inspection-only open that skips the recovery walk: it never takes the
  /// writer lock, never mutates, and does not refuse a directory whose log is
  /// damaged, so that verify() can report the damage precisely. Reads of damaged
  /// regions fail; derived queries may be incomplete because the derived index
  /// is not brought forward.
  kDiagnose = 4,
};

[[nodiscard]] std::string_view to_string(OpenMode mode) noexcept;

/// What to do when committed-prefix recovery finds bytes past the commit
/// watermark that no successful append ever acknowledged.
enum class RecoveryPolicy : std::uint8_t {
  /// Default. Discard the unacknowledged tail, preserving every committed
  /// record, and report the exact number of bytes and frames discarded. A tail
  /// frame was never acknowledged to any caller, so discarding it cannot lose
  /// committed state.
  kTruncateUncommittedTail = 1,
  /// Refuse to open a writable handle while an unacknowledged tail exists, so
  /// that an operator can inspect it first.
  kRefuseOnUncommittedTail = 2,
};

[[nodiscard]] std::string_view to_string(RecoveryPolicy policy) noexcept;

/// How duplicate submissions are detected.
enum class DuplicateDetection : std::uint8_t {
  /// Default. Every committed EventId and idempotency token is remembered for
  /// the lifetime of the ledger. Reaching the configured bound rejects further
  /// appends with kCapacityLimitExceeded rather than losing the guarantee.
  kExact = 1,
  /// Opt-in. Only the most recent entries are remembered; a duplicate older than
  /// the window is appended as a new event. Documented limitation.
  kWindowed = 2,
};

[[nodiscard]] std::string_view to_string(DuplicateDetection mode) noexcept;

/// Whether a read-only handle also takes a shared lock that excludes writers.
enum class ReaderLockPolicy : std::uint8_t {
  /// Default. Readers take no lock. A reader is safe beside a live writer
  /// because it only ever trusts frames at or below the commit watermark that a
  /// fully flushed manifest names.
  kNone = 1,
  /// Readers take a shared lock, which prevents any writer from opening the
  /// directory while a reader is attached.
  kShared = 2,
};

[[nodiscard]] std::string_view to_string(ReaderLockPolicy policy) noexcept;

/// Admission bounds, durability policy and recovery policy for one handle.
struct LedgerOptions {
  // -- Payload and batch bounds -----------------------------------------------
  /// Largest accepted event payload.
  std::size_t max_payload_bytes = 64u * 1024u;
  /// Largest accepted atomic batch.
  std::size_t max_batch_events = 1024;
  /// A segment is rotated automatically once it reaches this size.
  std::size_t max_segment_bytes = 64u * 1024u * 1024u;

  // -- Query bounds -----------------------------------------------------------
  /// Largest page a single query may request.
  std::size_t max_query_limit = 4096;
  /// Largest sequence window a query may walk without index postings.
  std::size_t max_scan_window = 1'000'000;

  // -- Derived-index bounds ---------------------------------------------------
  /// Maximum remembered EventId/idempotency entries. Exceeding it rejects the
  /// append; it never silently forgets an identity.
  std::size_t max_dedupe_entries = 1'000'000;
  /// Maximum resident query postings. Exceeding it stops adding postings and
  /// makes affected queries fall back to a bounded linear scan; it never makes a
  /// query return a wrong answer.
  std::size_t max_index_postings = 2'000'000;
  /// Maximum resident frame-offset entries. Exceeding it drops the offset table
  /// of the lowest-numbered segments, after which random access into those
  /// segments costs one bounded scan of that segment.
  std::size_t max_cached_frame_offsets = 16'000'000;
  /// Maximum registered subjects tracked by the admission state machine.
  std::size_t max_subjects = 1'000'000;
  /// Maximum distinct producing components tracked for stale-source rejection.
  std::size_t max_sources = 4096;

  // -- Checkpoints ------------------------------------------------------------
  /// Maximum retained checkpoint files. Older checkpoints are removed; a
  /// checkpoint is derived data, never provenance.
  std::size_t max_checkpoints = 8;
  /// A checkpoint is written automatically every this many committed events.
  /// Zero disables automatic checkpointing.
  std::size_t auto_checkpoint_interval = 10'000;

  // -- Export bounds ----------------------------------------------------------
  /// Largest number of events one canonical export call may emit.
  std::size_t max_export_events = 100'000;
  /// Largest byte size one canonical export call may emit.
  std::size_t max_export_bytes = 64u * 1024u * 1024u;

  // -- Durability -------------------------------------------------------------
  /// When true (default) a commit is published only after the segment flush and
  /// after the manifest replacement is flushed. When false the ledger still
  /// writes and orders identically but issues no flush, so a clean close and
  /// reopen is consistent while a host crash may lose acknowledged appends.
  /// Every handle reports this through LedgerState::durable and LedgerStats.
  bool durable_commits = true;
  /// Flush the containing directory after publishing a manifest replacement.
  bool flush_directory = true;

  // -- Policy -----------------------------------------------------------------
  RecoveryPolicy recovery = RecoveryPolicy::kTruncateUncommittedTail;
  DuplicateDetection duplicate_detection = DuplicateDetection::kExact;
  ReaderLockPolicy reader_lock = ReaderLockPolicy::kNone;

  // -- Injection --------------------------------------------------------------
  /// Monotonic source. Null selects SystemClock. The pointer must outlive the
  /// handle and must not be owned by it.
  IClock* clock = nullptr;
  /// Fault injector. Null disables injection. The pointer must outlive the
  /// handle.
  IFaultInjector* fault_injector = nullptr;

  // -- Determinism ------------------------------------------------------------
  /// Pins the informational wall-clock value written into new segment headers.
  /// Absent means "read the clock". Ordering never depends on this value.
  std::optional<std::uint64_t> segment_timestamp_unix_nanos;
};

/// One note recorded while opening or recovering a ledger.
struct RecoveryNote {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
};

/// What happened while opening a ledger directory.
struct OpenReport {
  bool created = false;
  bool recovered = false;
  /// Bytes of unacknowledged tail discarded to reach the committed prefix.
  std::uint64_t truncated_tail_bytes = 0;
  /// Frames discarded with that tail.
  std::uint64_t truncated_tail_frames = 0;
  /// The derived index was rebuilt from the authoritative log.
  bool index_rebuilt = false;
  /// Index postings past the commit watermark were discarded.
  bool index_truncated = false;
  /// The primary manifest was unusable and the previous publication was used.
  bool manifest_recovered_from_backup = false;
  /// Events replayed to bring derived state forward.
  std::uint64_t replayed_events = 0;
  std::vector<RecoveryNote> notes;
};

}  // namespace fsl
