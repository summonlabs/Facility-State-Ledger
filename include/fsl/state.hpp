// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fsl/counter.hpp"
#include "fsl/hash.hpp"
#include "fsl/ids.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file state.hpp
/// Read-only views of ledger structure: identity, watermark, segments,
/// checkpoints and operational statistics.
///
/// Everything here is a snapshot. A snapshot is a value: it stays valid and
/// self-consistent after the ledger advances, which is why these types are
/// returned by value rather than as references into live ledger state.

namespace fsl {

/// Identity and metadata of one ledger directory.
struct LedgerIdentity {
  LedgerId id;
  std::uint64_t segment_format_version = 0;
  std::uint64_t manifest_format_version = 0;
};

/// Authoritative position of the committed prefix.
struct CommitWatermark {
  /// Highest committed ledger sequence, absent when nothing is committed.
  std::optional<LedgerSequence> sequence;
  /// Logical tick of the last committed event.
  std::optional<LogicalTick> logical_tick;
  /// Segment that holds the last committed frame.
  std::optional<SegmentIndex> segment_index;
  /// Byte offset just past the last committed frame within that segment.
  std::uint64_t offset = 0;
  /// Running chain value through the last committed frame.
  Digest chain;
  /// Number of committed events.
  std::uint64_t event_count = 0;
  /// Publication counter of the manifest this watermark was read from.
  ManifestGeneration manifest_generation{1};
};

/// Snapshot of the admission-relevant authoritative state.
struct LedgerState {
  LedgerIdentity identity;
  CommitWatermark watermark;
  /// Current authoritative facility generation.
  FacilityGeneration facility_generation{1};
  /// The epoch that currently accepts mutations, absent when none is open.
  std::optional<FacilityEpoch> open_epoch;
  /// Highest epoch ever opened.
  std::optional<FacilityEpoch> latest_epoch;
  SegmentIndex active_segment{1};
  std::uint64_t segment_count = 1;
  /// Incarnation of the writable handle that owns this directory.
  WriterIncarnation writer_incarnation{1};
  /// True when the handle can mutate the ledger.
  bool writable = false;
  /// True when commits were published with a durability flush.
  bool durable = false;
  /// True after close() or after a failed open.
  bool closed = false;
};

/// Inspection view of one segment file.
struct SegmentInfo {
  SegmentIndex index{1};
  /// First ledger sequence stored in the segment; absent when it holds no events.
  std::optional<LedgerSequence> first_sequence;
  /// Last ledger sequence stored in the segment; absent when it holds no events.
  std::optional<LedgerSequence> last_sequence;
  /// Number of frames physically present, including a seal frame.
  std::uint64_t frame_count = 0;
  /// Number of committed events attributed to this segment.
  std::uint64_t event_count = 0;
  std::uint64_t byte_size = 0;
  /// True when the segment carries a valid seal frame.
  bool sealed = false;
  /// Chain value after the last frame that was validated.
  Digest chain;
  /// True when the segment passed a full integrity walk during this call.
  bool verified = false;
  std::string file_name;
};

/// Inspection view of one checkpoint file.
struct CheckpointInfo {
  CheckpointInfo(LedgerSequence sequence,
                 LogicalTick logical_tick,
                 SegmentIndex segment_index,
                 std::uint64_t offset,
                 Digest chain,
                 std::uint64_t event_count,
                 std::uint64_t created_unix_nanos,
                 std::string file_name)
      : sequence(sequence),
        logical_tick(logical_tick),
        segment_index(segment_index),
        offset(offset),
        chain(chain),
        event_count(event_count),
        created_unix_nanos(created_unix_nanos),
        file_name(std::move(file_name)) {}

  LedgerSequence sequence;
  LogicalTick logical_tick;
  SegmentIndex segment_index{1};
  std::uint64_t offset = 0;
  Digest chain;
  std::uint64_t event_count = 0;
  std::uint64_t created_unix_nanos = 0;
  std::string file_name;
};

/// Operational counters for one handle since it was opened.
///
/// These are diagnostics, not authoritative state, and are never persisted.
struct LedgerStats {
  std::uint64_t committed_events = 0;
  std::uint64_t committed_payload_bytes = 0;
  std::uint64_t committed_frames = 0;
  std::uint64_t segment_count = 1;
  /// Postings held in memory for bounded queries.
  std::uint64_t index_postings = 0;
  /// Registered dedupe entries held in memory.
  std::uint64_t dedupe_entries = 0;
  std::uint64_t subjects_known = 0;
  std::uint64_t sources_tracked = 0;
  std::uint64_t checkpoints = 0;
  /// Commit operations that reached the durable publication step.
  std::uint64_t durable_commits = 0;
  /// Durability flush calls issued by this handle.
  std::uint64_t flush_calls = 0;
  /// Appends that matched an existing event and were answered idempotently.
  std::uint64_t duplicate_appends = 0;
  /// Appends rejected by admission policy or validation.
  std::uint64_t rejected_appends = 0;
  /// True when every posting needed for indexed queries is resident.
  bool index_postings_complete = true;
  /// True when the handle publishes commits with flush semantics.
  bool durable = false;
  bool writable = false;
};

}  // namespace fsl
