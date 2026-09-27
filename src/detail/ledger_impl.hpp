// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "detail/derived.hpp"
#include "detail/frame.hpp"
#include "detail/paths.hpp"
#include "detail/platform.hpp"
#include "fsl/clock.hpp"
#include "fsl/event.hpp"
#include "fsl/ledger.hpp"
#include "fsl/options.hpp"
#include "fsl/payload.hpp"
#include "fsl/state.hpp"

namespace fsl::detail {

/// One open segment file plus its committed extent.
struct SegmentDescriptor {
  SegmentDescriptor(std::uint64_t segment_index,
                    std::uint64_t base,
                    std::uint64_t start_offset,
                    File handle)
      : index(segment_index),
        base_sequence(base),
        end_sequence(base == 0 ? 0 : base - 1),
        end_offset(start_offset),
        frame_count(0),
        sealed(false),
        file(std::move(handle)) {}

  SegmentDescriptor(const SegmentDescriptor&) = delete;
  SegmentDescriptor& operator=(const SegmentDescriptor&) = delete;

  std::uint64_t index = 0;
  std::uint64_t base_sequence = 0;
  std::atomic<std::uint64_t> end_sequence;
  std::atomic<std::uint64_t> end_offset;
  std::atomic<std::uint64_t> frame_count;
  std::atomic<bool> sealed;
  File file;
  /// Byte offset of each frame relative to the start of the segment, in frame
  /// order. Guarded by LedgerImpl::segments_mutex_; emptied when the resident
  /// offset budget is exceeded, after which lookups fall back to a bounded scan.
  std::vector<std::uint64_t> frame_offsets;
  bool offsets_available = true;
};

/// A frame located by scanning or by the offset table.
struct LocatedFrame {
  std::uint64_t segment_index = 0;
  std::uint64_t offset = 0;
  std::uint64_t frame_size = 0;
};

/// Shared implementation behind fsl::Ledger and fsl::ReplayStream.
///
/// The authoritative state is the manifest plus the committed prefix of the
/// segment log. Everything else the object holds is either configuration or
/// derived data that can be rebuilt from those two.
class LedgerImpl : public std::enable_shared_from_this<LedgerImpl> {
 public:
  static Result<std::shared_ptr<LedgerImpl>> open(const std::filesystem::path& directory,
                                                  OpenMode mode,
                                                  LedgerOptions options);

  ~LedgerImpl();
  LedgerImpl(const LedgerImpl&) = delete;
  LedgerImpl& operator=(const LedgerImpl&) = delete;

  // -- Mutation ---------------------------------------------------------------
  [[nodiscard]] Result<AppendOutcome> append(const SubmittedObservation& observation);
  [[nodiscard]] Result<BatchOutcome> append_batch(std::span<const SubmittedObservation> observations);
  [[nodiscard]] Result<void> rotate_segment();
  [[nodiscard]] Result<CheckpointInfo> create_checkpoint();
  /// Checkpoint creation with the mutation lock already held, for callers inside the commit path.
  [[nodiscard]] Result<CheckpointInfo> create_checkpoint_locked();
  /// Checkpoint creation with the mutation lock already held.
  [[nodiscard]] Result<void> flush();
  [[nodiscard]] Result<void> rebuild_index();
  [[nodiscard]] RecoveryReport recover(const RecoveryRequest& request);
  [[nodiscard]] Result<void> close();

  // -- Reads ------------------------------------------------------------------
  [[nodiscard]] Result<EventEnvelope> read(LedgerSequence sequence) const;
  [[nodiscard]] Result<EventEnvelope> find_event(EventId identity) const;
  [[nodiscard]] Result<EventEnvelope> find_idempotency(IdempotencyToken token) const;
  [[nodiscard]] Result<QueryPage> query(const Query& request) const;
  [[nodiscard]] Result<ReplayStream> make_replay(const ReplayRequest& request) const;
  [[nodiscard]] Result<std::uint64_t> replay_each(const ReplayRequest& request,
                                                  const ReplayVisitor& visitor) const;
  [[nodiscard]] VerifyReport verify(const VerifyRequest& request) const;

  // -- Snapshots --------------------------------------------------------------
  [[nodiscard]] Result<LedgerIdentity> identity() const;
  [[nodiscard]] Result<LedgerState> state() const;
  [[nodiscard]] Result<CommitWatermark> watermark() const;
  [[nodiscard]] LedgerStats stats() const;
  [[nodiscard]] const OpenReport& open_report() const noexcept { return open_report_; }
  [[nodiscard]] bool is_open() const noexcept { return !closed_.load(std::memory_order_acquire); }
  [[nodiscard]] bool is_writable() const noexcept { return writable_; }
  [[nodiscard]] bool is_durable() const noexcept { return durable_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return paths_.root(); }

  // -- Structure --------------------------------------------------------------
  [[nodiscard]] Result<std::vector<SegmentInfo>> segments() const;
  [[nodiscard]] Result<SegmentInfo> segment(SegmentIndex index) const;
  [[nodiscard]] Result<std::vector<CheckpointInfo>> checkpoints() const;
  [[nodiscard]] Result<CheckpointInfo> latest_checkpoint() const;
  [[nodiscard]] Result<SubjectView> subject(const SubjectRef& reference) const;
  [[nodiscard]] Result<std::vector<SubjectView>> subjects(std::size_t limit,
                                                          const std::optional<SubjectRef>& after) const;
  [[nodiscard]] Result<std::vector<SourceView>> sources(std::size_t limit) const;

  // -- Export -----------------------------------------------------------------
  [[nodiscard]] Result<void> export_audit(const AuditExportRequest& request, IAuditSink& sink) const;
  [[nodiscard]] Result<std::string> export_audit_json(const AuditExportRequest& request) const;

  /// Reads one committed frame without taking the mutation lock. Used by
  /// ReplayStream, which walks the immutable committed prefix sequentially.
  [[nodiscard]] Result<EventEnvelope> read_frame_at(std::uint64_t segment_index,
                                                    std::uint64_t offset,
                                                    std::uint64_t& frame_size) const;

  /// Snapshot of the committed upper bound, safe to read without the lock.
  [[nodiscard]] std::uint64_t committed_upper_bound() const noexcept {
    return committed_sequence_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t committed_lower_bound() const noexcept {
    return first_sequence_.load(std::memory_order_acquire);
  }

 private:
  LedgerImpl(std::filesystem::path root, OpenMode mode, LedgerOptions options);

  struct PlannedEvent {
    explicit PlannedEvent(SubmittedObservation value) : observation(std::move(value)) {}

    SubmittedObservation observation;
    std::uint64_t sequence = 0;
    std::uint64_t logical_tick = 0;
    std::uint64_t monotonic_nanos = 0;
    /// True when this submission duplicates a committed event and nothing new is
    /// written for it.
    bool duplicate = false;
  };

  // -- Open helpers -----------------------------------------------------------
  [[nodiscard]] Status initialize();
  [[nodiscard]] Status load_manifest();
  [[nodiscard]] Status load_segments();
  [[nodiscard]] Status validate_committed_prefix(bool full_scan, bool truncate_tail);
  [[nodiscard]] Status load_index();
  [[nodiscard]] Status replay_range_into_state(std::uint64_t from_sequence, std::uint64_t to_sequence);
  [[nodiscard]] Status rebuild_index_from_log(OpenReport* report);
  [[nodiscard]] Status initialize_new_ledger();
  [[nodiscard]] Status start_index_generation(std::uint64_t generation);
  /// Rebuilds the constant-size derived admission state from index postings,
  /// used after entries past the commit watermark have been dropped.
  void recompute_derived_from_index();
  [[nodiscard]] Result<EventEnvelope> read_event_at(std::uint64_t segment_index, std::uint64_t offset) const;
  [[nodiscard]] std::vector<IndexEntry> index_entries_for(const EventEnvelope& envelope) const;
  void fold_entry_into_derived(const IndexEntry& entry);
  [[nodiscard]] Status read_manifest_file(const std::filesystem::path& path, Manifest& out) const;
  [[nodiscard]] Status validate_options() const;

  /// Identities already planned inside the batch under construction, so that a
  /// batch cannot contain the same identity twice.
  struct PendingIdentities {
    std::unordered_map<EventId, std::size_t> event_ids;
    std::unordered_map<IdempotencyToken, std::size_t> tokens;
  };

  // -- Commit path ------------------------------------------------------------
  [[nodiscard]] Status admit(const SubmittedObservation& observation) const;
  [[nodiscard]] Status plan_observation(const SubmittedObservation& observation,
                                        std::uint64_t sequence,
                                        std::uint64_t logical_tick,
                                        const PendingIdentities* pending,
                                        PlannedEvent& planned) const;
  [[nodiscard]] Status plan_batch(std::span<const SubmittedObservation> observations,
                                  std::vector<PlannedEvent>& planned,
                                  std::size_t& duplicate_count) const;
  [[nodiscard]] Status commit_planned(const std::vector<PlannedEvent>& planned,
                                      std::vector<EventEnvelope>& committed,
                                      std::size_t& duplicate_count);
  [[nodiscard]] std::vector<std::uint8_t> encode_envelope_body(const PlannedEvent& entry) const;
  [[nodiscard]] Result<EventEnvelope> make_envelope(const PlannedEvent& entry) const;
  [[nodiscard]] Status publish_manifest(bool check_authority);
  [[nodiscard]] Status append_index_entries(const std::vector<IndexEntry>& entries);
  [[nodiscard]] Status open_segment(std::uint64_t segment_index, std::uint64_t base_sequence);
  [[nodiscard]] Status seal_active_segment();
  /// Rotation performed with the mutation lock already held.
  [[nodiscard]] Status rotate_locked();
  [[nodiscard]] Status verify_manifest_authority() const;
  [[nodiscard]] Status flush_index(bool force);
  [[nodiscard]] Status write_checkpoint_file(const Checkpoint& checkpoint);
  [[nodiscard]] Status prune_checkpoints();

  // -- Derived-state probes ---------------------------------------------------
  [[nodiscard]] std::optional<bool> subject_is_live(const SubjectRef& reference) const;
  [[nodiscard]] bool subject_is_known(const SubjectRef& reference) const;
  [[nodiscard]] bool event_exists(const EventId& identity) const;
  [[nodiscard]] bool relationship_is_asserted(const SubjectRef& subject,
                                              RelationshipKind kind,
                                              const SubjectRef& related) const;
  void inject(Boundary boundary) const;

  // -- Read helpers -----------------------------------------------------------
  [[nodiscard]] Result<EventEnvelope> read_committed(LedgerSequence sequence) const;
  [[nodiscard]] Result<LocatedFrame> locate(LedgerSequence sequence) const;
  /// Fills a segment's frame-offset table on first access, honouring the
  /// resident-offset budget. Returns false when the table could not be cached,
  /// in which case the caller falls back to a bounded scan of that segment.
  [[nodiscard]] bool ensure_frame_offsets(const std::shared_ptr<SegmentDescriptor>& descriptor) const;
  void enforce_frame_offset_budget() const;
  [[nodiscard]] std::shared_ptr<const std::vector<std::shared_ptr<SegmentDescriptor>>> segment_snapshot() const;
  [[nodiscard]] std::shared_ptr<SegmentDescriptor> segment_for(std::uint64_t sequence) const;

  // -- Verification -----------------------------------------------------------
  struct WalkResult {
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t end_offset = 0;
    Digest chain;
    std::optional<VerifyFinding> finding;
  };
  [[nodiscard]] WalkResult walk_segment(const SegmentDescriptor& descriptor,
                                        std::uint64_t start_offset,
                                        std::uint64_t expected_sequence,
                                        const Digest& start_chain,
                                        std::uint64_t stop_offset,
                                        std::size_t max_records,
                                        bool expect_commit_at_stop,
                                        std::vector<std::uint64_t>* offsets) const;
  [[nodiscard]] VerifyReport verify_index_consistency(const VerifyReport& base) const;
  /// Deterministic identity of the ledger's own creation record.
  [[nodiscard]] static EventId derive_creation_event_id(const LedgerId& identity) noexcept;

  void set_committed_state(std::uint64_t sequence,
                           std::uint64_t logical_tick,
                           std::uint64_t segment_index,
                           std::uint64_t offset,
                           std::uint64_t frame_index,
                           const Digest& chain,
                           std::uint64_t event_count);

  [[nodiscard]] Status request_recovery(const std::string& message) const;
  void note(ErrorCode code, std::string message);

  std::filesystem::path root_;
  Paths paths_;
  OpenMode mode_ = OpenMode::kReadWrite;
  LedgerOptions options_;
  bool writable_ = false;
  bool durable_ = true;
  bool index_enabled_ = true;

  SystemClock default_clock_;
  IClock* clock_ = nullptr;

  mutable std::mutex mutex_;
  mutable std::shared_mutex segments_mutex_;
  FileLock writer_lock_;
  Manifest manifest_;
  ManifestGeneration manifest_generation_{1};
  /// False while the published manifest is known to be unusable, so that the
  /// authority probe cannot block the publication that repairs it.
  bool manifest_authority_trusted_ = true;
  WriterIncarnation writer_incarnation_{1};

  File active_file_;
  SegmentHeader active_header_;
  Digest active_chain_;
  std::uint64_t active_offset_ = 0;
  std::uint64_t active_record_index_ = 0;
  std::uint64_t active_first_sequence_ = 0;

  std::vector<std::shared_ptr<SegmentDescriptor>> segments_;
  /// Frame-offset tables are a read-side cache, so the resident counter is
  /// mutable and the cache is filled from const read paths.
  mutable std::size_t resident_frame_offsets_ = 0;
  /// Reused buffer for frame encoding, so a commit does not allocate per event.
  std::vector<std::uint8_t> scratch_frame_;

  DerivedIndex index_;
  File index_file_;
  std::uint64_t index_generation_ = 1;
  std::uint64_t index_ordinal_ = 1;
  std::uint64_t index_entry_count_ = 0;
  std::uint64_t index_durable_through_ = 0;
  bool index_file_dirty_ = false;

  DerivedState derived_;
  std::uint64_t events_since_checkpoint_ = 0;

  OpenReport open_report_;

  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> committed_sequence_{0};
  std::atomic<std::uint64_t> committed_logical_tick_{0};
  std::atomic<std::uint64_t> committed_segment_{0};
  std::atomic<std::uint64_t> committed_offset_{0};
  std::atomic<std::uint64_t> committed_frame_index_{0};
  std::atomic<std::uint64_t> first_sequence_{1};
  std::atomic<std::uint64_t> event_count_{0};

  mutable std::mutex stats_mutex_;
  LedgerStats stats_;
};

/// Buffered forward reader over the immutable committed prefix.
class SequentialFrameReader {
 public:
  SequentialFrameReader(std::shared_ptr<const std::vector<std::shared_ptr<SegmentDescriptor>>> segments,
                        std::uint64_t segment_index,
                        std::uint64_t offset,
                        std::uint64_t first_sequence,
                        std::uint64_t last_sequence);

  /// Returns false at end of range. `error` is set when a frame could not be
  /// read or failed validation.
  [[nodiscard]] bool next(std::optional<EventEnvelope>& out, Status& error);

  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

 private:
  [[nodiscard]] bool refill();

  std::shared_ptr<const std::vector<std::shared_ptr<SegmentDescriptor>>> segments_;
  std::size_t segment_position_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t next_sequence_ = 0;
  std::uint64_t last_sequence_ = 0;
  std::vector<std::uint8_t> buffer_;
  std::size_t buffer_used_ = 0;
  std::size_t buffer_position_ = 0;
  bool exhausted_ = false;
};

}  // namespace fsl::detail
