// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fsl/bytes.hpp"
#include "fsl/hash.hpp"
#include "fsl/ids.hpp"
#include "fsl/status.hpp"

/// \file frame.hpp
/// Internal physical framing shared by ledger segments, index segments and
/// checkpoints. This is not part of the public API: consumers see EventEnvelope,
/// SegmentInfo and CheckpointInfo instead.
///
/// Frame layout (little-endian, no padding):
///
///   offset 0   u32  frame_size        total bytes of the frame, >= 72
///   offset 4   u32  frame_crc32c      CRC-32C over bytes [8, frame_size)
///   offset 8   u8   frame_kind
///   offset 9   u8   frame_version
///   offset 10  u16  frame_flags       reserved, must be zero
///   offset 12  u64  record_index      1-based within the segment
///   offset 20  u64  ledger_sequence   0 when the kind carries no sequence
///   offset 28  u64  logical_tick      0 when the kind carries no tick
///   offset 36  32B  chain             running chain after this frame
///   offset 68  ...  body              frame_size - 72 bytes
///   offset N   4B   commit_marker     0x54494D43 ("CMIT"); the last four bytes
///
/// A frame is *committed* only when all of the following hold: the full
/// declared length is present, frame_flags is zero, the trailing commit marker
/// is exact, the CRC matches, and the chain field equals the running chain
/// recomputed from the previous frame. Trailing bytes that do not satisfy this
/// are an uncommitted tail and are truncated by recovery under documented rules.

namespace fsl::detail {

inline constexpr std::uint32_t kCommitMarker = 0x54494D43u;  // "CMIT" little-endian
inline constexpr std::uint8_t kFrameFormatVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 68;
inline constexpr std::size_t kFrameTrailerSize = 4;
inline constexpr std::size_t kFrameOverhead = kFrameHeaderSize + kFrameTrailerSize;
inline constexpr std::uint32_t kMaxFrameSize = 256u * 1024u * 1024u;

/// Discriminant of a physical frame.
enum class FrameKind : std::uint8_t {
  kEvent = 1,
  kSegmentSeal = 2,
  kIndexEntry = 3,
  kIndexSeal = 4,
};

[[nodiscard]] std::string_view to_string(FrameKind kind) noexcept;

/// Header fields of a decoded frame.
struct FrameHeader {
  std::uint32_t frame_size = 0;
  std::uint32_t frame_crc32c = 0;
  FrameKind kind = FrameKind::kEvent;
  std::uint8_t version = 0;
  std::uint16_t flags = 0;
  std::uint64_t record_index = 0;
  std::uint64_t ledger_sequence = 0;
  std::uint64_t logical_tick = 0;
  Digest chain;
};

/// Result of attempting to decode one frame from a byte range.
enum class FrameDecodeOutcome {
  /// A complete, integrity-verified frame occupies the range.
  kOk,
  /// The range holds fewer bytes than the frame declares. Only meaningful at
  /// the end of a file: an uncommitted or torn tail.
  kIncomplete,
  /// A complete-length frame failed validation.
  kCorrupt,
  /// The frame declares a format version this build does not implement.
  kUnsupportedVersion,
};

struct DecodedFrame {
  FrameHeader header;
  std::span<const std::uint8_t> body;
};

/// Decodes one frame starting at `bytes[0]`.
///
/// Returns kIncomplete when `bytes` is shorter than the declared frame size,
/// which includes the case where fewer than kFrameOverhead bytes are available.
[[nodiscard]] FrameDecodeOutcome decode_frame(std::span<const std::uint8_t> bytes, DecodedFrame& out) noexcept;

/// Encodes one frame, appending it to `out`.
///
/// `prev_chain` is the chain value of the preceding frame in the same segment,
/// or the segment chain seed for the first frame.
void encode_frame(FrameKind kind,
                  std::uint64_t record_index,
                  std::uint64_t ledger_sequence,
                  std::uint64_t logical_tick,
                  std::span<const std::uint8_t> body,
                  const Digest& prev_chain,
                  std::vector<std::uint8_t>& out);

/// Computes the running chain for a frame: SHA-256 over the previous chain
/// followed by the frame bytes with the crc and chain fields zeroed.
[[nodiscard]] Digest compute_frame_chain(const Digest& prev_chain, std::span<const std::uint8_t> frame_bytes) noexcept;

/// Reads the chain field out of already-encoded frame bytes. Returns the zero
/// digest when fewer than kFrameHeaderSize bytes are supplied.
[[nodiscard]] Digest frame_chain_of(std::span<const std::uint8_t> frame_bytes) noexcept;

// -- Segment header -----------------------------------------------------------

inline constexpr std::uint64_t kSegmentFormatVersion = 1;
inline constexpr std::size_t kSegmentHeaderSize = 64;
/// Eight bytes. Declared as a character array with an explicit length so that
/// the embedded padding bytes are part of the magic rather than being truncated
/// by a NUL-terminated conversion.
inline constexpr char kSegmentMagicBytes[8] = {'F', 'S', 'L', 'S', 'E', 'G', '\0', '\0'};
inline constexpr std::string_view kSegmentMagic(kSegmentMagicBytes, sizeof(kSegmentMagicBytes));

struct SegmentHeader {
  std::uint64_t format_version = kSegmentFormatVersion;
  LedgerId ledger_id;
  std::uint64_t segment_index = 0;
  std::uint64_t base_sequence = 0;
  std::uint64_t created_unix_nanos = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_segment_header(const SegmentHeader& header);
/// Decodes and verifies a segment header. Reports kUnsupportedFormatVersion for
/// a recognised magic with an unimplemented version.
[[nodiscard]] Result<SegmentHeader> decode_segment_header(std::span<const std::uint8_t> bytes);

/// Chain seed for the first frame of a segment. Binds the segment's chain to the
/// ledger identity and segment position so that frames cannot be moved between
/// ledgers or between segment indexes undetected.
[[nodiscard]] Digest segment_chain_seed(const SegmentHeader& header) noexcept;

/// Body of a segment seal frame, written when a segment is rotated.
struct SegmentSealBody {
  std::uint64_t record_count = 0;   ///< frames written before the seal
  std::uint64_t first_sequence = 0; ///< 0 when the segment holds no events
  std::uint64_t last_sequence = 0;  ///< 0 when the segment holds no events
  std::uint64_t sealed_offset = 0;  ///< byte offset of the seal frame itself
};

[[nodiscard]] std::vector<std::uint8_t> encode_segment_seal(const SegmentSealBody& body);
[[nodiscard]] Result<SegmentSealBody> decode_segment_seal(std::span<const std::uint8_t> body);

// -- Manifest -----------------------------------------------------------------

inline constexpr std::uint64_t kManifestFormatVersion = 1;
inline constexpr char kManifestMagicBytes[8] = {'F', 'S', 'L', 'M', 'A', 'N', '\0', '\0'};
inline constexpr std::string_view kManifestMagic(kManifestMagicBytes, sizeof(kManifestMagicBytes));

/// Bit flags persisted in the manifest.
enum ManifestFlags : std::uint64_t {
  /// Commits were published with the configured durability flush behaviour.
  kManifestFlagDurable = 1u << 0,
};

/// Authoritative commit watermark plus derived-ledger bookkeeping.
struct Manifest {
  std::uint64_t format_version = kManifestFormatVersion;
  std::uint64_t manifest_generation = 1;
  std::uint64_t writer_incarnation = 1;
  LedgerId ledger_id;
  std::uint64_t flags = kManifestFlagDurable;

  /// Position of the last committed frame. A committed sequence of 0 means the
  /// ledger holds no committed events yet.
  std::uint64_t committed_sequence = 0;
  std::uint64_t committed_logical_tick = 0;
  std::uint64_t committed_segment_index = 0;
  std::uint64_t committed_offset = 0;
  std::uint64_t committed_frame_index = 0;
  Digest committed_chain;

  std::uint64_t active_segment_index = 1;
  std::uint64_t active_segment_offset = 0;
  std::uint64_t segment_count = 1;

  std::uint64_t total_event_count = 0;

  std::uint64_t facility_generation = 1;
  std::uint64_t open_epoch = 0;  ///< 0 encodes "no epoch is open"
  std::uint64_t latest_epoch = 0;

  std::uint64_t index_generation = 1;
  std::uint64_t index_through_sequence = 0;

  std::uint64_t checkpoint_count = 0;
  std::uint64_t latest_checkpoint_sequence = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_manifest(const Manifest& manifest);
[[nodiscard]] Result<Manifest> decode_manifest(std::span<const std::uint8_t> bytes);

// -- Checkpoint ---------------------------------------------------------------

inline constexpr std::uint64_t kCheckpointFormatVersion = 1;
inline constexpr char kCheckpointMagicBytes[8] = {'F', 'S', 'L', 'C', 'K', 'P', '\0', '\0'};
inline constexpr std::string_view kCheckpointMagic(kCheckpointMagicBytes, sizeof(kCheckpointMagicBytes));

/// An integrity and replay anchor at a committed sequence.
///
/// A checkpoint is derived data: it never authorises an event that the manifest
/// does not already name, and a checkpoint that claims to be ahead of the
/// committed prefix is rejected rather than trusted.
struct Checkpoint {
  std::uint64_t format_version = kCheckpointFormatVersion;
  LedgerId ledger_id;
  std::uint64_t sequence = 0;
  std::uint64_t logical_tick = 0;
  std::uint64_t segment_index = 0;
  std::uint64_t offset = 0;
  Digest chain_at_sequence;
  std::uint64_t manifest_generation = 0;
  std::uint64_t facility_generation = 0;
  std::uint64_t open_epoch = 0;
  std::uint64_t latest_epoch = 0;
  std::uint64_t event_count = 0;
  std::uint64_t created_unix_nanos = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_checkpoint(const Checkpoint& checkpoint);
[[nodiscard]] Result<Checkpoint> decode_checkpoint(std::span<const std::uint8_t> bytes);

// -- Index entries ------------------------------------------------------------

inline constexpr std::uint64_t kIndexFormatVersion = 1;
inline constexpr std::uint64_t kIndexSealFormatVersion = 1;

/// Discriminant of a derived index posting.
enum class IndexKind : std::uint8_t {
  /// key = EventId bytes; extra = EventKind.
  kEventId = 1,
  /// key = IdempotencyToken bytes.
  kIdempotency = 2,
  /// key = canonical SubjectRef bytes; extra = EventKind.
  kSubject = 3,
  /// key = source component id text; extra = SourceGeneration, aux = SourceSequence (0 absent).
  kSource = 4,
  /// key = u64 epoch; extra = EventKind.
  kEpoch = 5,
  /// key = u64 facility generation; extra = EventKind.
  kGeneration = 6,
  /// key = u8 EventKind.
  kEventKind = 7,
  /// key = canonical relationship triple; extra = EventKind.
  kRelationship = 8,
};

[[nodiscard]] std::string_view to_string(IndexKind kind) noexcept;

/// Body of an index entry frame.
struct IndexEntry {
  IndexKind kind = IndexKind::kEventId;
  std::uint64_t sequence = 0;
  std::uint64_t extra = 0;
  std::uint64_t aux = 0;
  std::vector<std::uint8_t> key;
};

[[nodiscard]] std::vector<std::uint8_t> encode_index_entry(const IndexEntry& entry);
[[nodiscard]] Result<IndexEntry> decode_index_entry(std::span<const std::uint8_t> body);

/// Body of an index seal frame: how far the index has been brought forward.
struct IndexSealBody {
  std::uint64_t entry_count = 0;
  std::uint64_t through_sequence = 0;
  std::uint64_t index_generation = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_index_seal(const IndexSealBody& body);
[[nodiscard]] Result<IndexSealBody> decode_index_seal(std::span<const std::uint8_t> body);

}  // namespace fsl::detail
