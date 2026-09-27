// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "detail/frame.hpp"

#include <cstring>

#include "fsl/text.hpp"

namespace fsl::detail {
namespace {

// Byte offsets inside a frame, derived from the layout documented in frame.hpp.
constexpr std::size_t kOffsetFrameSize = 0;
constexpr std::size_t kOffsetFrameCrc = 4;
constexpr std::size_t kOffsetFrameKind = 8;
constexpr std::size_t kOffsetFrameVersion = 9;
constexpr std::size_t kOffsetFrameFlags = 10;
constexpr std::size_t kOffsetRecordIndex = 12;
constexpr std::size_t kOffsetSequence = 20;
constexpr std::size_t kOffsetLogicalTick = 28;
constexpr std::size_t kOffsetChain = 36;

static_assert(kOffsetChain + Digest::kSize == kFrameHeaderSize, "frame header layout drifted");

void store_u32(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(data[shift / 8]) << shift;
  }
  return value;
}

[[nodiscard]] bool is_known_frame_kind(std::uint8_t raw) noexcept {
  return raw >= static_cast<std::uint8_t>(FrameKind::kEvent) &&
         raw <= static_cast<std::uint8_t>(FrameKind::kIndexSeal);
}

/// Verifies the trailing CRC and digest of a self-describing record.
[[nodiscard]] Status verify_record_envelope(std::span<const std::uint8_t> bytes,
                                            std::size_t digest_offset,
                                            ErrorCode crc_code,
                                            ErrorCode digest_code,
                                            std::string_view what) {
  if (bytes.size() < Digest::kSize + 4) {
    return malformed_input(ErrorCode::kTruncatedInput, std::string(what) + " is too short");
  }
  const std::size_t crc_offset = digest_offset - 4;
  const std::uint32_t stored_crc = load_u32(bytes.data() + crc_offset);
  const std::uint32_t actual_crc = crc32c(bytes.data(), crc_offset);
  if (stored_crc != actual_crc) {
    return integrity_failure(crc_code, std::string(what) + " checksum mismatch");
  }
  Digest stored;
  if (!ByteReader(bytes.subspan(digest_offset)).digest(stored)) {
    return malformed_input(ErrorCode::kTruncatedInput, std::string(what) + " digest is truncated");
  }
  if (stored != Sha256::hash(bytes.data(), digest_offset)) {
    return integrity_failure(digest_code, std::string(what) + " digest mismatch");
  }
  return Status::ok();
}

}  // namespace

std::string_view to_string(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::kEvent:
      return "event";
    case FrameKind::kSegmentSeal:
      return "segment-seal";
    case FrameKind::kIndexEntry:
      return "index-entry";
    case FrameKind::kIndexSeal:
      return "index-seal";
  }
  return "unknown";
}

std::string_view to_string(IndexKind kind) noexcept {
  switch (kind) {
    case IndexKind::kEventId:
      return "event-id";
    case IndexKind::kIdempotency:
      return "idempotency";
    case IndexKind::kSubject:
      return "subject";
    case IndexKind::kSource:
      return "source";
    case IndexKind::kEpoch:
      return "epoch";
    case IndexKind::kGeneration:
      return "generation";
    case IndexKind::kEventKind:
      return "event-kind";
    case IndexKind::kRelationship:
      return "relationship";
  }
  return "unknown";
}

Digest compute_frame_chain(const Digest& prev_chain, std::span<const std::uint8_t> frame_bytes) noexcept {
  // The chain covers the frame with its own derived fields removed, so that the
  // value can be computed before the checksum and chain fields are written and
  // re-derived from the finished bytes during verification.
  Sha256 hasher;
  hasher.update(prev_chain.data(), Digest::kSize);
  if (frame_bytes.size() < kFrameHeaderSize) {
    hasher.update(frame_bytes.data(), frame_bytes.size());
    return hasher.finish();
  }
  constexpr std::uint8_t kZeroWord[4] = {0, 0, 0, 0};
  constexpr std::uint8_t kZeroDigest[Digest::kSize] = {};
  hasher.update(frame_bytes.data(), kOffsetFrameCrc);
  hasher.update(kZeroWord, sizeof(kZeroWord));
  hasher.update(frame_bytes.data() + kOffsetFrameKind, kOffsetChain - kOffsetFrameKind);
  hasher.update(kZeroDigest, sizeof(kZeroDigest));
  hasher.update(frame_bytes.data() + kFrameHeaderSize, frame_bytes.size() - kFrameHeaderSize);
  return hasher.finish();
}

Digest frame_chain_of(std::span<const std::uint8_t> frame_bytes) noexcept {
  if (frame_bytes.size() < kFrameHeaderSize) {
    return Digest{};
  }
  return Digest::from_bytes(frame_bytes.data() + kOffsetChain);
}

void encode_frame(FrameKind kind,
                  std::uint64_t record_index,
                  std::uint64_t ledger_sequence,
                  std::uint64_t logical_tick,
                  std::span<const std::uint8_t> body,
                  const Digest& prev_chain,
                  std::vector<std::uint8_t>& out) {
  const std::size_t start = out.size();
  const std::size_t total = kFrameOverhead + body.size();
  const auto frame_size = static_cast<std::uint32_t>(total);

  ByteWriter writer(out);
  writer.u32(frame_size);
  writer.u32(0);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u8(kFrameFormatVersion);
  writer.u16(0);
  writer.u64(record_index);
  writer.u64(ledger_sequence);
  writer.u64(logical_tick);
  writer.digest(Digest{});
  writer.bytes(body);
  writer.u32(kCommitMarker);

  const std::span<std::uint8_t> frame(out.data() + start, total);
  const Digest chain = compute_frame_chain(prev_chain, frame);
  std::memcpy(out.data() + start + kOffsetChain, chain.data(), Digest::kSize);
  const std::uint32_t crc = crc32c(out.data() + start + 8, total - 8);
  store_u32(out, start + kOffsetFrameCrc, crc);
}

FrameDecodeOutcome decode_frame(std::span<const std::uint8_t> bytes, DecodedFrame& out) noexcept {
  if (bytes.size() < kFrameOverhead) {
    return FrameDecodeOutcome::kIncomplete;
  }
  const std::uint32_t frame_size = load_u32(bytes.data() + kOffsetFrameSize);
  if (frame_size < kFrameOverhead || frame_size > kMaxFrameSize) {
    return FrameDecodeOutcome::kCorrupt;
  }
  if (bytes.size() < frame_size) {
    return FrameDecodeOutcome::kIncomplete;
  }
  const std::span<const std::uint8_t> frame = bytes.first(frame_size);

  const std::uint8_t version = frame[kOffsetFrameVersion];
  if (version != kFrameFormatVersion) {
    return FrameDecodeOutcome::kUnsupportedVersion;
  }
  if (!is_known_frame_kind(frame[kOffsetFrameKind])) {
    return FrameDecodeOutcome::kCorrupt;
  }
  if (frame[kOffsetFrameFlags] != 0 || frame[kOffsetFrameFlags + 1] != 0) {
    return FrameDecodeOutcome::kCorrupt;
  }
  const std::uint32_t stored_crc = load_u32(frame.data() + kOffsetFrameCrc);
  if (crc32c(frame.data() + 8, frame_size - 8) != stored_crc) {
    return FrameDecodeOutcome::kCorrupt;
  }

  ByteReader reader(frame);
  // The trailer holds the commit marker; check it explicitly so that a frame
  // truncated exactly at the marker is rejected rather than accepted.
  std::uint32_t marker = 0;
  for (int i = 0; i < 4; ++i) {
    marker |= static_cast<std::uint32_t>(frame[frame_size - 4 + static_cast<std::size_t>(i)]) << (8 * i);
  }
  if (marker != kCommitMarker) {
    return FrameDecodeOutcome::kCorrupt;
  }

  std::uint32_t declared_size = 0;
  std::uint32_t declared_crc = 0;
  std::uint8_t kind_raw = 0;
  std::uint8_t version_raw = 0;
  std::uint16_t flags = 0;
  if (!reader.u32(declared_size) || !reader.u32(declared_crc) || !reader.u8(kind_raw) ||
      !reader.u8(version_raw) || !reader.u16(flags) || !reader.u64(out.header.record_index) ||
      !reader.u64(out.header.ledger_sequence) || !reader.u64(out.header.logical_tick) ||
      !reader.digest(out.header.chain)) {
    return FrameDecodeOutcome::kCorrupt;
  }
  if (out.header.record_index == 0) {
    return FrameDecodeOutcome::kCorrupt;
  }

  out.header.frame_size = frame_size;
  out.header.frame_crc32c = stored_crc;
  out.header.kind = static_cast<FrameKind>(kind_raw);
  out.header.version = version_raw;
  out.header.flags = flags;
  out.body = frame.subspan(kFrameHeaderSize, frame_size - kFrameOverhead);
  return FrameDecodeOutcome::kOk;
}

// -- Segment header -----------------------------------------------------------

std::vector<std::uint8_t> encode_segment_header(const SegmentHeader& header) {
  std::vector<std::uint8_t> out;
  out.reserve(kSegmentHeaderSize);
  ByteWriter writer(out);
  writer.bytes(kSegmentMagic);
  writer.u32(static_cast<std::uint32_t>(header.format_version));
  writer.u16(static_cast<std::uint16_t>(kSegmentHeaderSize));
  writer.u16(0);
  writer.id128(header.ledger_id);
  writer.u64(header.segment_index);
  writer.u64(header.base_sequence);
  writer.u64(header.created_unix_nanos);
  writer.u32(0);
  const std::uint32_t crc = crc32c(out.data(), out.size());
  writer.u32(crc);
  return out;
}

Result<SegmentHeader> decode_segment_header(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kSegmentHeaderSize) {
    return malformed_input(ErrorCode::kTruncatedInput, "segment header is truncated");
  }
  const std::span<const std::uint8_t> fixed = bytes.first(kSegmentHeaderSize);
  if (!std::equal(kSegmentMagic.begin(), kSegmentMagic.end(), fixed.begin())) {
    return malformed_input(ErrorCode::kBadMagic, "segment header magic is not FSLSEG");
  }
  ByteReader reader(fixed);
  std::span<const std::uint8_t> magic;
  std::uint32_t version = 0;
  std::uint16_t header_size = 0;
  std::uint16_t flags = 0;
  SegmentHeader header;
  std::uint32_t reserved = 0;
  std::uint32_t stored_crc = 0;
  const bool parsed = reader.take(8, magic) && reader.u32(version) && reader.u16(header_size) &&
                      reader.u16(flags) && reader.id128(header.ledger_id) &&
                      reader.u64(header.segment_index) && reader.u64(header.base_sequence) &&
                      reader.u64(header.created_unix_nanos) && reader.u32(reserved) &&
                      reader.u32(stored_crc);
  if (!parsed) {
    return malformed_input(ErrorCode::kTruncatedInput, "segment header is truncated");
  }
  if (header_size != kSegmentHeaderSize) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "segment header size field is " + std::to_string(header_size) + ", expected " +
                               std::to_string(kSegmentHeaderSize));
  }
  if (flags != 0 || reserved != 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "segment header reserved fields are not zero");
  }
  if (crc32c(fixed.data(), kSegmentHeaderSize - 4) != stored_crc) {
    return integrity_failure(ErrorCode::kHeaderChecksumMismatch, "segment header checksum mismatch");
  }
  if (version != kSegmentFormatVersion) {
    return unsupported_version(ErrorCode::kUnsupportedFormatVersion,
                               "segment format version " + std::to_string(version) + " is not supported");
  }
  header.format_version = version;
  if (header.segment_index == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "segment index zero is not a valid segment");
  }
  if (header.ledger_id.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "segment header carries a nil ledger identity");
  }
  return header;
}

Digest segment_chain_seed(const SegmentHeader& header) noexcept {
  Sha256 hasher;
  hasher.update(header.ledger_id.data(), LedgerId::kByteSize);
  std::uint8_t scratch[24];
  for (int i = 0; i < 8; ++i) {
    scratch[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((header.segment_index >> (8 * i)) & 0xFFu);
    scratch[static_cast<std::size_t>(8 + i)] =
        static_cast<std::uint8_t>((header.base_sequence >> (8 * i)) & 0xFFu);
    scratch[static_cast<std::size_t>(16 + i)] =
        static_cast<std::uint8_t>((header.format_version >> (8 * i)) & 0xFFu);
  }
  hasher.update(scratch, sizeof(scratch));
  return hasher.finish();
}

std::vector<std::uint8_t> encode_segment_seal(const SegmentSealBody& body) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.u64(body.record_count);
  writer.u64(body.first_sequence);
  writer.u64(body.last_sequence);
  writer.u64(body.sealed_offset);
  return out;
}

Result<SegmentSealBody> decode_segment_seal(std::span<const std::uint8_t> body) {
  ByteReader reader(body);
  SegmentSealBody seal;
  if (!reader.u64(seal.record_count) || !reader.u64(seal.first_sequence) || !reader.u64(seal.last_sequence) ||
      !reader.u64(seal.sealed_offset)) {
    return malformed_input(ErrorCode::kTruncatedInput, "segment seal frame is truncated");
  }
  if (!reader.at_end()) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes, "segment seal frame has trailing bytes");
  }
  return seal;
}

// -- Manifest -----------------------------------------------------------------

namespace {

constexpr std::size_t kManifestFieldsSize = 216;

}  // namespace

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest) {
  std::vector<std::uint8_t> out;
  out.reserve(kManifestFieldsSize + 36);
  ByteWriter writer(out);
  writer.bytes(kManifestMagic);
  writer.u32(static_cast<std::uint32_t>(manifest.format_version));
  writer.u32(static_cast<std::uint32_t>(kManifestFieldsSize + 36));
  writer.u64(manifest.manifest_generation);
  writer.u64(manifest.writer_incarnation);
  writer.id128(manifest.ledger_id);
  writer.u64(manifest.flags);
  writer.u64(manifest.committed_sequence);
  writer.u64(manifest.committed_logical_tick);
  writer.u64(manifest.committed_segment_index);
  writer.u64(manifest.committed_offset);
  writer.u64(manifest.committed_frame_index);
  writer.digest(manifest.committed_chain);
  writer.u64(manifest.active_segment_index);
  writer.u64(manifest.active_segment_offset);
  writer.u64(manifest.segment_count);
  writer.u64(manifest.total_event_count);
  writer.u64(manifest.facility_generation);
  writer.u64(manifest.open_epoch);
  writer.u64(manifest.latest_epoch);
  writer.u64(manifest.index_generation);
  writer.u64(manifest.index_through_sequence);
  writer.u64(manifest.checkpoint_count);
  writer.u64(manifest.latest_checkpoint_sequence);
  const std::uint32_t crc = crc32c(out.data(), out.size());
  writer.u32(crc);
  const Digest digest = Sha256::hash(out.data(), out.size());
  writer.digest(digest);
  return out;
}

Result<Manifest> decode_manifest(std::span<const std::uint8_t> bytes) {
  constexpr std::size_t kTotal = kManifestFieldsSize + 4 + Digest::kSize;
  if (bytes.size() < kTotal) {
    return malformed_input(ErrorCode::kTruncatedInput, "manifest is truncated");
  }
  const std::span<const std::uint8_t> fixed = bytes.first(kTotal);
  if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), fixed.begin())) {
    return malformed_input(ErrorCode::kBadMagic, "manifest magic is not FSLMAN");
  }
  ByteReader reader(fixed);
  std::span<const std::uint8_t> magic;
  std::uint32_t version = 0;
  std::uint32_t size = 0;
  Manifest manifest;
  std::uint32_t crc = 0;
  const bool parsed = reader.take(8, magic) && reader.u32(version) && reader.u32(size) &&
                      reader.u64(manifest.manifest_generation) && reader.u64(manifest.writer_incarnation) &&
                      reader.id128(manifest.ledger_id) && reader.u64(manifest.flags) &&
                      reader.u64(manifest.committed_sequence) && reader.u64(manifest.committed_logical_tick) &&
                      reader.u64(manifest.committed_segment_index) && reader.u64(manifest.committed_offset) &&
                      reader.u64(manifest.committed_frame_index) && reader.digest(manifest.committed_chain) &&
                      reader.u64(manifest.active_segment_index) && reader.u64(manifest.active_segment_offset) &&
                      reader.u64(manifest.segment_count) && reader.u64(manifest.total_event_count) &&
                      reader.u64(manifest.facility_generation) && reader.u64(manifest.open_epoch) &&
                      reader.u64(manifest.latest_epoch) && reader.u64(manifest.index_generation) &&
                      reader.u64(manifest.index_through_sequence) && reader.u64(manifest.checkpoint_count) &&
                      reader.u64(manifest.latest_checkpoint_sequence);
  if (!parsed) {
    return malformed_input(ErrorCode::kTruncatedInput, "manifest is truncated");
  }
  if (!reader.u32(crc)) {
    return malformed_input(ErrorCode::kTruncatedInput, "manifest is truncated");
  }
  if (size != kTotal) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "manifest size field is " + std::to_string(size) + ", expected " +
                               std::to_string(kTotal));
  }
  if (crc32c(fixed.data(), kManifestFieldsSize) != crc) {
    return integrity_failure(ErrorCode::kHeaderChecksumMismatch, "manifest checksum mismatch");
  }
  Digest digest;
  if (!reader.digest(digest)) {
    return malformed_input(ErrorCode::kTruncatedInput, "manifest digest is truncated");
  }
  if (digest != Sha256::hash(fixed.data(), kManifestFieldsSize + 4)) {
    return integrity_failure(ErrorCode::kDigestMismatch, "manifest digest mismatch");
  }
  if (version != kManifestFormatVersion) {
    return unsupported_version(ErrorCode::kUnsupportedFormatVersion,
                               "manifest format version " + std::to_string(version) + " is not supported");
  }
  manifest.format_version = version;
  if (manifest.ledger_id.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "manifest carries a nil ledger identity");
  }
  if (manifest.manifest_generation == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "manifest generation zero is not valid");
  }
  if (manifest.committed_sequence != 0 && manifest.committed_segment_index == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "manifest commits a sequence without naming its segment");
  }
  if (manifest.segment_count == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "manifest names zero segments");
  }
  if (manifest.open_epoch != 0 && manifest.open_epoch > manifest.latest_epoch) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "manifest open epoch is ahead of the latest epoch");
  }
  if (manifest.index_through_sequence > manifest.committed_sequence) {
    return integrity_failure(ErrorCode::kManifestAheadOfLog,
                             "manifest index watermark is ahead of the commit watermark");
  }
  return manifest;
}

// -- Checkpoint ---------------------------------------------------------------

std::vector<std::uint8_t> encode_checkpoint(const Checkpoint& checkpoint) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.bytes(kCheckpointMagic);
  writer.u32(static_cast<std::uint32_t>(checkpoint.format_version));
  writer.u32(0);  // size patched below
  writer.id128(checkpoint.ledger_id);
  writer.u64(checkpoint.sequence);
  writer.u64(checkpoint.logical_tick);
  writer.u64(checkpoint.segment_index);
  writer.u64(checkpoint.offset);
  writer.digest(checkpoint.chain_at_sequence);
  writer.u64(checkpoint.manifest_generation);
  writer.u64(checkpoint.facility_generation);
  writer.u64(checkpoint.open_epoch);
  writer.u64(checkpoint.latest_epoch);
  writer.u64(checkpoint.event_count);
  writer.u64(checkpoint.created_unix_nanos);
  const auto total = static_cast<std::uint32_t>(out.size() + 4 + Digest::kSize);
  store_u32(out, 12, total);
  const std::uint32_t crc = crc32c(out.data(), out.size());
  writer.u32(crc);
  const Digest digest = Sha256::hash(out.data(), out.size());
  writer.digest(digest);
  return out;
}

Result<Checkpoint> decode_checkpoint(std::span<const std::uint8_t> bytes) {
  constexpr std::size_t kMinimum = 16 + (16 + 8 * 8 + Digest::kSize) + 4 + Digest::kSize;
  if (bytes.size() < kMinimum) {
    return malformed_input(ErrorCode::kTruncatedInput, "checkpoint is truncated");
  }
  if (!std::equal(kCheckpointMagic.begin(), kCheckpointMagic.end(), bytes.begin())) {
    return malformed_input(ErrorCode::kBadMagic, "checkpoint magic is not FSLCKP");
  }
  ByteReader reader(bytes);
  std::span<const std::uint8_t> magic;
  std::uint32_t version = 0;
  std::uint32_t size = 0;
  Checkpoint checkpoint;
  if (!reader.take(8, magic) || !reader.u32(version) || !reader.u32(size) ||
      !reader.id128(checkpoint.ledger_id) || !reader.u64(checkpoint.sequence) ||
      !reader.u64(checkpoint.logical_tick) || !reader.u64(checkpoint.segment_index) ||
      !reader.u64(checkpoint.offset) || !reader.digest(checkpoint.chain_at_sequence) ||
      !reader.u64(checkpoint.manifest_generation) || !reader.u64(checkpoint.facility_generation) ||
      !reader.u64(checkpoint.open_epoch) || !reader.u64(checkpoint.latest_epoch) ||
      !reader.u64(checkpoint.event_count) || !reader.u64(checkpoint.created_unix_nanos)) {
    return malformed_input(ErrorCode::kTruncatedInput, "checkpoint is truncated");
  }
  if (size != bytes.size()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "checkpoint size field is " + std::to_string(size) + " but " +
                               std::to_string(bytes.size()) + " bytes are present");
  }
  const std::size_t digest_offset = bytes.size() - Digest::kSize;
  const Status envelope = verify_record_envelope(bytes, digest_offset, ErrorCode::kHeaderChecksumMismatch,
                                                 ErrorCode::kDigestMismatch, "checkpoint");
  if (envelope.is_error()) {
    return envelope;
  }
  if (reader.offset() + 4 != digest_offset) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes, "checkpoint has unexpected trailing bytes");
  }
  if (version != kCheckpointFormatVersion) {
    return unsupported_version(ErrorCode::kUnsupportedFormatVersion,
                               "checkpoint format version " + std::to_string(version) + " is not supported");
  }
  checkpoint.format_version = version;
  if (checkpoint.ledger_id.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "checkpoint carries a nil ledger identity");
  }
  if (checkpoint.sequence == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "checkpoint sequence zero is not valid");
  }
  return checkpoint;
}

// -- Index --------------------------------------------------------------------

std::vector<std::uint8_t> encode_index_entry(const IndexEntry& entry) {
  std::vector<std::uint8_t> out;
  out.reserve(32 + entry.key.size());
  ByteWriter writer(out);
  writer.u8(static_cast<std::uint8_t>(entry.kind));
  writer.u8(0);
  writer.u16(0);
  writer.u64(entry.sequence);
  writer.u64(entry.extra);
  writer.u64(entry.aux);
  writer.u32(static_cast<std::uint32_t>(entry.key.size()));
  writer.bytes(entry.key);
  return out;
}

Result<IndexEntry> decode_index_entry(std::span<const std::uint8_t> body) {
  ByteReader reader(body);
  std::uint8_t kind_raw = 0;
  std::uint8_t reserved = 0;
  std::uint16_t reserved16 = 0;
  IndexEntry entry;
  if (!reader.u8(kind_raw) || !reader.u8(reserved) || !reader.u16(reserved16) ||
      !reader.u64(entry.sequence) || !reader.u64(entry.extra) || !reader.u64(entry.aux)) {
    return malformed_input(ErrorCode::kTruncatedInput, "index entry is truncated");
  }
  if (reserved != 0 || reserved16 != 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "index entry reserved fields are not zero");
  }
  if (kind_raw < static_cast<std::uint8_t>(IndexKind::kEventId) ||
      kind_raw > static_cast<std::uint8_t>(IndexKind::kRelationship)) {
    return malformed_input(ErrorCode::kInvalidEnumValue,
                           "index entry kind " + std::to_string(kind_raw) + " is not known");
  }
  entry.kind = static_cast<IndexKind>(kind_raw);
  std::span<const std::uint8_t> key;
  if (!reader.sized_bytes(key, 4096)) {
    return malformed_input(ErrorCode::kSizeOutOfRange, "index entry key length is out of range");
  }
  if (!reader.at_end()) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes, "index entry has trailing bytes");
  }
  entry.key.assign(key.begin(), key.end());

  // Fixed-size kinds carry exactly one key width; variable-size kinds carry a
  // bounded one. A key whose width does not match its kind is malformed rather
  // than merely surprising, so it is rejected here.
  const std::size_t exact = [&]() -> std::size_t {
    switch (entry.kind) {
      case IndexKind::kEventId:
      case IndexKind::kIdempotency:
        return EventId::kByteSize;
      case IndexKind::kEpoch:
      case IndexKind::kGeneration:
        return 8;
      case IndexKind::kEventKind:
        return 1;
      case IndexKind::kSource:
      case IndexKind::kSubject:
      case IndexKind::kRelationship:
        return 0;  // variable width
    }
    return 0;
  }();
  const std::size_t maximum = [&]() -> std::size_t {
    switch (entry.kind) {
      case IndexKind::kEventId:
      case IndexKind::kIdempotency:
        return EventId::kByteSize;
      case IndexKind::kEpoch:
      case IndexKind::kGeneration:
        return 8;
      case IndexKind::kEventKind:
        return 1;
      case IndexKind::kSource:
        return SourceComponentId::kMaxLength;
      case IndexKind::kSubject:
      case IndexKind::kRelationship:
        return 4096;
    }
    return 0;
  }();
  const bool wrong_width =
      entry.key.empty() || entry.key.size() > maximum || (exact != 0 && entry.key.size() != exact);
  if (wrong_width) {
    return malformed_input(ErrorCode::kSizeOutOfRange,
                           "index entry key length " + std::to_string(entry.key.size()) +
                               " is invalid for kind " + std::string(to_string(entry.kind)));
  }  if (entry.sequence == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "index entry names sequence zero");
  }
  return entry;
}

std::vector<std::uint8_t> encode_index_seal(const IndexSealBody& body) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.u64(body.entry_count);
  writer.u64(body.through_sequence);
  writer.u64(body.index_generation);
  return out;
}

Result<IndexSealBody> decode_index_seal(std::span<const std::uint8_t> body) {
  ByteReader reader(body);
  IndexSealBody seal;
  if (!reader.u64(seal.entry_count) || !reader.u64(seal.through_sequence) || !reader.u64(seal.index_generation)) {
    return malformed_input(ErrorCode::kTruncatedInput, "index seal frame is truncated");
  }
  if (!reader.at_end()) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes, "index seal frame has trailing bytes");
  }
  return seal;
}

}  // namespace fsl::detail
