// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fsl/counter.hpp"
#include "fsl/event.hpp"
#include "fsl/hash.hpp"
#include "fsl/ids.hpp"
#include "fsl/options.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file query.hpp
/// Bounded queries, deterministic replay, integrity verification and canonical
/// audit export.
///
/// A query never returns an unbounded amount of work: `limit` is mandatory, the
/// result set is capped by the handle's max_query_limit, and a query that cannot
/// use index postings refuses a window wider than max_scan_window instead of
/// walking an arbitrarily large ledger.

namespace fsl {

/// Where a query obtained its candidate sequences.
enum class QuerySource : std::uint8_t {
  /// Resident index postings narrowed the search.
  kIndexed = 1,
  /// No resident postings covered the predicate; the ledger range was scanned.
  kLinearScan = 2,
};

[[nodiscard]] std::string_view to_string(QuerySource source) noexcept;

/// A bounded historical query.
///
/// Predicates combine with AND. `from` and `to` are inclusive ledger sequence
/// bounds. `after` is a resumption cursor: it is equivalent to, and applied
/// after, `from`.
struct Query {
  std::optional<LedgerSequence> from;
  std::optional<LedgerSequence> to;
  std::optional<LedgerSequence> after;
  std::optional<EventId> event_id;
  std::optional<SubjectRef> subject;
  std::optional<SourceComponentId> source;
  std::optional<FacilityGeneration> facility_generation;
  std::optional<FacilityEpoch> epoch;
  std::optional<EventKind> kind;
  /// Mandatory. Zero is rejected with kLimitRequired.
  std::size_t limit = 0;
};

/// One page of query results in ascending ledger sequence order.
struct QueryPage {
  std::vector<EventEnvelope> events;
  /// True when the predicate has no further matches after this page.
  bool exhausted = true;
  /// Cursor to pass as Query::after for the next page. Absent when exhausted.
  std::optional<LedgerSequence> next_cursor;
  QuerySource source = QuerySource::kIndexed;
  /// Number of ledger records examined to produce the page.
  std::uint64_t records_examined = 0;
};

/// Deterministic replay request.
struct ReplayRequest {
  /// First sequence to emit. Absent starts at the first committed event.
  std::optional<LedgerSequence> from;
  /// Last sequence to emit (inclusive). Absent ends at the commit watermark
  /// captured when the stream was created.
  std::optional<LedgerSequence> to;
  /// Optional filters. Replay never skips a sequence silently: a filtered-out
  /// event is simply not delivered, and the stream still walks every sequence in
  /// the requested range.
  std::optional<EventKind> kind;
  std::optional<SubjectRef> subject;
  std::optional<SourceComponentId> source;
  /// Hard cap on delivered events. Zero means "no cap beyond the range".
  std::size_t max_events = 0;
};

/// Outcome of one ReplayStream advance.
enum class ReplayStep : std::uint8_t {
  /// An event was delivered.
  kEvent = 1,
  /// The requested range is exhausted.
  kEnd = 2,
  /// The ledger could not be read; see ReplayStream::error().
  kError = 3,
};

/// Scope of an integrity verification.
enum class VerifyScope : std::uint8_t {
  /// Validate the manifest and the frames that reach the commit watermark.
  kManifestTail = 1,
  /// Walk every committed frame from the first segment.
  kFull = 2,
  /// Walk from a checkpoint anchor to the commit watermark.
  kFromCheckpoint = 3,
};

[[nodiscard]] std::string_view to_string(VerifyScope scope) noexcept;

/// One integrity observation.
struct VerifyFinding {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  std::optional<LedgerSequence> sequence;
  std::optional<SegmentIndex> segment_index;
  std::uint64_t offset = 0;
};

struct VerifyRequest {
  VerifyScope scope = VerifyScope::kFull;
  /// Checkpoint to start from when scope is kFromCheckpoint.
  std::optional<LedgerSequence> checkpoint_sequence;
  /// Verify that the derived index agrees with the authoritative log.
  bool verify_index = true;
  /// Stop after this many records. Zero means "no limit".
  std::size_t max_records = 0;
};

/// Result of an integrity verification. Always returned, never thrown.
struct VerifyReport {
  /// ok, or the first integrity failure encountered.
  Status status;
  std::uint64_t records_verified = 0;
  std::uint64_t bytes_verified = 0;
  std::uint64_t segments_verified = 0;
  std::optional<LedgerSequence> first_sequence;
  std::optional<LedgerSequence> last_sequence;
  Digest chain_at_end;
  bool index_verified = false;
  bool index_consistent = true;
  std::vector<VerifyFinding> findings;

  [[nodiscard]] bool ok() const noexcept { return status.is_ok(); }
};

/// Explicit recovery request.
struct RecoveryRequest {
  /// Report what would be done without modifying anything.
  bool dry_run = false;
  /// Rebuild the derived index from the authoritative log even if it looks
  /// consistent.
  bool rebuild_index = false;
};

/// Result of an explicit recovery pass.
struct RecoveryReport {
  Status status;
  bool dry_run = false;
  bool truncated = false;
  std::uint64_t truncated_tail_bytes = 0;
  std::uint64_t truncated_tail_frames = 0;
  bool index_rebuilt = false;
  bool manifest_recovered_from_backup = false;
  std::uint64_t replayed_events = 0;
  std::vector<RecoveryNote> notes;
};

/// Canonical audit export bounds.
struct AuditExportRequest {
  std::optional<LedgerSequence> from;
  std::optional<LedgerSequence> to;
  /// Largest number of records to emit. Absent uses max_export_events.
  std::optional<std::size_t> max_events;
  /// Emit only records whose subject is this reference.
  std::optional<SubjectRef> subject;
};

/// Destination for streamed canonical audit records.
///
/// write() receives complete records, each already terminated by a newline.
/// Returning false aborts the export with kIoFailure; the sink is responsible
/// for its own error reporting.
class IAuditSink {
 public:
  IAuditSink() = default;
  IAuditSink(const IAuditSink&) = delete;
  IAuditSink& operator=(const IAuditSink&) = delete;
  IAuditSink(IAuditSink&&) = delete;
  IAuditSink& operator=(IAuditSink&&) = delete;
  virtual ~IAuditSink() = default;
  virtual bool write(std::string_view record) = 0;
};

}  // namespace fsl
