// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fsl/event.hpp"
#include "fsl/options.hpp"
#include "fsl/query.hpp"
#include "fsl/state.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file ledger.hpp
/// The public Facility State Ledger API.
///
/// A Ledger handle owns one ledger directory. The authoritative state of that
/// directory is a manifest that names the committed prefix of an append-only
/// segment log; everything else the ledger keeps in memory (subject registry,
/// source watermarks, dedupe identities, query postings) is derived from that
/// log and is rebuilt from it after any restart.
///
/// Concurrency model
/// -----------------
///  * Within a process, a handle is safe to use from multiple threads. Reads
///    (read/find/query/replay/verify/export) never take the mutation lock and
///    observe a consistent committed prefix: an append publishes bytes past the
///    watermark and only then advances it atomically.
///  * Mutations (append/append_batch/rotate/checkpoint/recover/rebuild_index)
///    are serialised by one internal lock.
///  * Across processes, exactly one writable handle may exist for a directory.
///    The writer takes an exclusive operating-system lock for the lifetime of
///    the handle; a second writable handle fails with kLocked.
///  * Read-only handles take no lock by default and are safe beside a live
///    writer, for the reason above. ReaderLockPolicy::kShared makes readers
///    exclude writers instead.

namespace fsl {

namespace detail {
class LedgerImpl;
}  // namespace detail

/// Result of appending one submission.
struct AppendOutcome {
  AppendOutcome(EventEnvelope event, bool duplicate) : event(std::move(event)), duplicate(duplicate) {}

  /// The committed event. For an idempotent duplicate this is the event that was
  /// already committed, not a new one.
  EventEnvelope event;
  /// True when the submission matched an already-committed event and nothing was
  /// written.
  bool duplicate = false;
};

/// Result of appending one atomic batch. Either every submission is committed in
/// order, or none is.
struct BatchOutcome {
  std::vector<EventEnvelope> events;
  /// Number of submissions answered idempotently from committed state.
  std::uint64_t duplicates = 0;
  /// True when every submission was a duplicate and nothing was written.
  bool nothing_written = false;
};

/// Forward-only, deterministic replay over a fixed sequence range.
///
/// A stream fixes its upper bound when it is created, so a concurrent writer
/// cannot extend it. Events are delivered in strictly ascending ledger sequence
/// with no gaps in the walked range.
class ReplayStream {
 public:
  ReplayStream(ReplayStream&& other) noexcept;
  ReplayStream& operator=(ReplayStream&& other) noexcept;
  ReplayStream(const ReplayStream&) = delete;
  ReplayStream& operator=(const ReplayStream&) = delete;
  ~ReplayStream();

  /// Advances to the next delivered event.
  ///
  /// On kEvent the optional holds the event. On kEnd it is empty and the stream
  /// is finished. On kError it is empty and error() explains why.
  [[nodiscard]] ReplayStep next(std::optional<EventEnvelope>& out);
  /// Error set when next() returned kError.
  [[nodiscard]] const Status& error() const noexcept;
  /// Sequence that the next call to next() will consider.
  [[nodiscard]] std::optional<LedgerSequence> next_sequence() const noexcept;
  /// Inclusive upper bound fixed at creation.
  [[nodiscard]] std::optional<LedgerSequence> upper_bound() const noexcept;
  /// Events delivered so far.
  [[nodiscard]] std::uint64_t delivered() const noexcept;

 private:
  friend class Ledger;
  friend class detail::LedgerImpl;
  struct Impl;
  explicit ReplayStream(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

/// Visitor invoked by Ledger::replay_each. Returning false stops the replay
/// early. The visitor runs with no ledger lock held and may read from, query, or
/// append to the same ledger; it must not destroy the ledger it is called from.
using ReplayVisitor = std::function<bool(const EventEnvelope&)>;

/// Inspection view of one subject's lifecycle in the ledger.
struct SubjectView {
  explicit SubjectView(SubjectRef subject) : subject(std::move(subject)) {}

  SubjectRef subject;
  bool registered = false;
  bool retired = false;
  std::optional<LedgerSequence> first_sequence;
  std::optional<LedgerSequence> last_sequence;
  std::uint64_t event_count = 0;
};

/// Inspection view of one producing component's watermark.
struct SourceView {
  SourceView(SourceComponentId source, SourceGeneration generation)
      : source(std::move(source)), generation(generation) {}

  SourceComponentId source;
  SourceGeneration generation{1};
  std::optional<SourceSequence> last_source_sequence;
  std::optional<LedgerSequence> last_sequence;
  std::uint64_t event_count = 0;
};

/// Durable, provenance-preserving ledger of authoritative facility-state events.
///
/// A handle is move-only. Destroying a handle releases the writer lock and
/// closes files; it is equivalent to calling close() and discarding the status.
class Ledger {
 public:
  /// Opens or creates a ledger directory.
  ///
  /// kReadWrite/kCreate take the exclusive writer lock. kCreate creates the
  /// directory and, if it holds no ledger, initialises a new one whose first
  /// committed event is a kLedgerOpened record at sequence 1.
  [[nodiscard]] static Result<Ledger> open(const std::filesystem::path& directory,
                                           OpenMode mode = OpenMode::kReadWrite,
                                           LedgerOptions options = {});

  /// Convenience for open(directory, OpenMode::kCreate, options).
  [[nodiscard]] static Result<Ledger> create(const std::filesystem::path& directory,
                                             LedgerOptions options = {});

  Ledger(Ledger&& other) noexcept;
  Ledger& operator=(Ledger&& other) noexcept;
  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;
  ~Ledger();

  // -- Identity and snapshots -------------------------------------------------

  /// Identity of the open ledger. Absent on a moved-from or closed handle.
  [[nodiscard]] Result<LedgerIdentity> identity() const;
  /// Snapshot of the authoritative admission state.
  [[nodiscard]] Result<LedgerState> state() const;
  /// Snapshot of the committed prefix.
  [[nodiscard]] Result<CommitWatermark> watermark() const;
  /// Operational counters since this handle was opened.
  [[nodiscard]] LedgerStats stats() const;
  /// What happened while this handle was opened.
  [[nodiscard]] const OpenReport& open_report() const noexcept;
  /// True while the handle is open and usable.
  [[nodiscard]] bool is_open() const noexcept;
  /// True when the handle may mutate the ledger.
  [[nodiscard]] bool is_writable() const noexcept;
  /// True when commits are published with durability flushes.
  [[nodiscard]] bool is_durable() const noexcept;
  /// Absolute, canonicalised directory this handle owns.
  [[nodiscard]] std::filesystem::path directory() const;

  // -- Mutation ---------------------------------------------------------------

  /// Validates, admits and durably commits one submission.
  ///
  /// Returns kReadOnly on a read-only handle. A submission whose EventId, or
  /// whose idempotency token, matches a committed event with identical content is
  /// answered with AppendOutcome::duplicate set and nothing written; the same
  /// identity with different content is rejected with kDuplicateEventId or
  /// kDuplicateIdempotencyToken.
  [[nodiscard]] Result<AppendOutcome> append(const SubmittedObservation& observation);

  /// Validates, admits and durably commits a batch atomically.
  ///
  /// Either every submission becomes a committed event, in the order given, or
  /// none does. An empty batch is rejected with kBatchEmpty and one larger than
  /// max_batch_events with kBatchTooLarge.
  [[nodiscard]] Result<BatchOutcome> append_batch(std::span<const SubmittedObservation> observations);

  /// Seals the active segment and starts a new one.
  [[nodiscard]] Result<void> rotate_segment();

  /// Publishes a checkpoint at the current commit watermark.
  [[nodiscard]] Result<CheckpointInfo> create_checkpoint();

  /// Flushes pending derived-index state. Committed events are already durable.
  [[nodiscard]] Result<void> flush();

  /// Discards derived index state and rebuilds it from the authoritative log.
  [[nodiscard]] Result<void> rebuild_index();

  /// Runs recovery explicitly on an open writable handle.
  [[nodiscard]] RecoveryReport recover(const RecoveryRequest& request = {});

  // -- Reads ------------------------------------------------------------------

  /// Reads one committed event by ledger sequence.
  [[nodiscard]] Result<EventEnvelope> read(LedgerSequence sequence) const;

  /// Finds a committed event by identity.
  [[nodiscard]] Result<EventEnvelope> find_event(EventId event_id) const;

  /// Finds a committed event by caller-supplied idempotency token.
  [[nodiscard]] Result<EventEnvelope> find_idempotency(IdempotencyToken token) const;

  /// Runs a bounded historical query.
  [[nodiscard]] Result<QueryPage> query(const Query& request) const;

  /// Creates a deterministic replay stream.
  [[nodiscard]] Result<ReplayStream> replay(const ReplayRequest& request = {}) const;

  /// Replays a range through a visitor, returning how many events were delivered.
  [[nodiscard]] Result<std::uint64_t> replay_each(const ReplayRequest& request,
                                                  const ReplayVisitor& visitor) const;

  /// Verifies integrity of the committed prefix, and optionally of the index.
  [[nodiscard]] VerifyReport verify(const VerifyRequest& request = {}) const;

  // -- Structure --------------------------------------------------------------

  /// Metadata for every segment, ascending by segment index.
  [[nodiscard]] Result<std::vector<SegmentInfo>> segments() const;
  /// Metadata for one segment, validated by walking its frames.
  [[nodiscard]] Result<SegmentInfo> segment(SegmentIndex index) const;
  /// Retained checkpoints, ascending by sequence.
  [[nodiscard]] Result<std::vector<CheckpointInfo>> checkpoints() const;
  /// Most recent retained checkpoint.
  [[nodiscard]] Result<CheckpointInfo> latest_checkpoint() const;
  /// Lifecycle view of one subject.
  [[nodiscard]] Result<SubjectView> subject(const SubjectRef& reference) const;
  /// Registered subjects in ascending SubjectRef order, at most `limit`.
  [[nodiscard]] Result<std::vector<SubjectView>> subjects(std::size_t limit,
                                                          const std::optional<SubjectRef>& after = std::nullopt) const;
  /// Tracked producing components in ascending SourceComponentId order.
  [[nodiscard]] Result<std::vector<SourceView>> sources(std::size_t limit) const;

  // -- Export -----------------------------------------------------------------

  /// Streams canonical audit records into `sink`.
  [[nodiscard]] Result<void> export_audit(const AuditExportRequest& request, IAuditSink& sink) const;
  /// Materialises canonical audit records as one newline-terminated string.
  [[nodiscard]] Result<std::string> export_audit_json(const AuditExportRequest& request = {}) const;

  // -- Lifecycle --------------------------------------------------------------

  /// Flushes derived state, releases the writer lock and closes all files.
  ///
  /// close() is idempotent. Every method except is_open() and stats() reports
  /// kClosed after a successful close.
  [[nodiscard]] Result<void> close();

 private:
  explicit Ledger(std::shared_ptr<detail::LedgerImpl> impl) noexcept;

  std::shared_ptr<detail::LedgerImpl> impl_;
};

}  // namespace fsl
