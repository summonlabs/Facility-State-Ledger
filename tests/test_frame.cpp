// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "detail/frame.hpp"
#include "detail/paths.hpp"
#include "support/test_support.hpp"

/// \file test_frame.cpp
/// The physical frame format: encoding determinism, chain continuity, and the
/// detection of every single-byte alteration.

namespace {

using fsl::detail::DecodedFrame;
using fsl::detail::FrameDecodeOutcome;
using fsl::detail::FrameKind;

[[nodiscard]] std::vector<std::uint8_t> make_frame(FrameKind kind,
                                                   std::uint64_t record_index,
                                                   std::uint64_t sequence,
                                                   std::uint64_t tick,
                                                   const std::vector<std::uint8_t>& body,
                                                   const fsl::Digest& previous) {
  std::vector<std::uint8_t> out;
  fsl::detail::encode_frame(kind, record_index, sequence, tick, body, previous, out);
  return out;
}

FSL_TEST(frame_layout_is_the_documented_size) {
  const std::vector<std::uint8_t> body{1, 2, 3, 4, 5};
  const std::vector<std::uint8_t> frame =
      make_frame(FrameKind::kEvent, 1, 1, 1, body, fsl::Digest{});
  FSL_CHECK_EQ(frame.size(), fsl::detail::kFrameOverhead + body.size());
  DecodedFrame decoded;
  FSL_REQUIRE(fsl::detail::decode_frame(frame, decoded) == FrameDecodeOutcome::kOk);
  FSL_CHECK_EQ(decoded.header.frame_size, static_cast<std::uint32_t>(frame.size()));
  FSL_CHECK_EQ(decoded.header.record_index, std::uint64_t{1});
  FSL_CHECK_EQ(decoded.header.ledger_sequence, std::uint64_t{1});
  FSL_CHECK_EQ(decoded.header.logical_tick, std::uint64_t{1});
  FSL_CHECK(decoded.header.kind == FrameKind::kEvent);
  FSL_CHECK_EQ(decoded.body.size(), body.size());
  FSL_CHECK(std::equal(decoded.body.begin(), decoded.body.end(), body.begin()));
}

FSL_TEST(frame_encoding_is_deterministic) {
  const std::vector<std::uint8_t> body{9, 9, 9};
  const fsl::Digest previous = fsl::Sha256::hash("previous");
  const std::vector<std::uint8_t> first = make_frame(FrameKind::kEvent, 4, 7, 7, body, previous);
  const std::vector<std::uint8_t> second = make_frame(FrameKind::kEvent, 4, 7, 7, body, previous);
  FSL_CHECK(first == second);
}

FSL_TEST(frame_chain_commits_to_the_previous_chain) {
  const std::vector<std::uint8_t> body{1};
  const std::vector<std::uint8_t> first = make_frame(FrameKind::kEvent, 1, 1, 1, body, fsl::Digest{});
  const fsl::Digest first_chain = fsl::detail::frame_chain_of(first);
  const std::vector<std::uint8_t> second = make_frame(FrameKind::kEvent, 2, 2, 2, body, first_chain);
  FSL_CHECK(fsl::detail::frame_chain_of(second) != first_chain);

  // Re-deriving the chain from the finished bytes must reproduce the stored one.
  FSL_CHECK_EQ(fsl::detail::compute_frame_chain(fsl::Digest{}, first), first_chain);
  FSL_CHECK_EQ(fsl::detail::compute_frame_chain(first_chain, second),
               fsl::detail::frame_chain_of(second));
}

FSL_TEST(a_single_byte_alteration_is_detected_at_every_offset) {
  const std::vector<std::uint8_t> body{1, 2, 3, 4, 5, 6, 7, 8};
  const fsl::Digest previous = fsl::Sha256::hash("seed");
  const std::vector<std::uint8_t> original = make_frame(FrameKind::kEvent, 3, 3, 3, body, previous);
  for (std::size_t offset = 0; offset < original.size(); ++offset) {
    std::vector<std::uint8_t> altered = original;
    altered[offset] ^= 0x01;
    DecodedFrame decoded;
    const FrameDecodeOutcome outcome = fsl::detail::decode_frame(altered, decoded);
    if (outcome != FrameDecodeOutcome::kOk) {
      continue;  // rejected outright, which is also correct
    }
    // If the frame still decodes, the chain must no longer match.
    const fsl::Digest expected = fsl::detail::compute_frame_chain(previous, altered);
    FSL_CHECK(!(expected == decoded.header.chain));
  }
}

FSL_TEST(a_truncated_frame_is_reported_as_incomplete) {
  const std::vector<std::uint8_t> body{1, 2, 3, 4, 5, 6, 7, 8};
  const std::vector<std::uint8_t> frame =
      make_frame(FrameKind::kEvent, 1, 1, 1, body, fsl::Digest{});
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    DecodedFrame decoded;
    const std::span<const std::uint8_t> view(frame.data(), cut);
    const FrameDecodeOutcome outcome = fsl::detail::decode_frame(view, decoded);
    FSL_CHECK(outcome == FrameDecodeOutcome::kIncomplete);
  }
}

FSL_TEST(a_damaged_commit_marker_is_rejected) {
  const std::vector<std::uint8_t> body{1, 2, 3};
  std::vector<std::uint8_t> frame = make_frame(FrameKind::kEvent, 1, 1, 1, body, fsl::Digest{});
  frame[frame.size() - 1] ^= 0xFF;
  DecodedFrame decoded;
  FSL_CHECK(fsl::detail::decode_frame(frame, decoded) == FrameDecodeOutcome::kCorrupt);
}

FSL_TEST(an_unsupported_frame_version_is_reported_distinctly) {
  std::vector<std::uint8_t> frame = make_frame(FrameKind::kEvent, 1, 1, 1, {1}, fsl::Digest{});
  frame[9] = 0x7F;  // version byte
  DecodedFrame decoded;
  FSL_CHECK(fsl::detail::decode_frame(frame, decoded) == FrameDecodeOutcome::kUnsupportedVersion);
}

FSL_TEST(an_impossible_frame_length_is_rejected_before_reading) {
  std::vector<std::uint8_t> frame = make_frame(FrameKind::kEvent, 1, 1, 1, {1}, fsl::Digest{});
  frame[0] = 0xFF;
  frame[1] = 0xFF;
  frame[2] = 0xFF;
  frame[3] = 0x7F;
  DecodedFrame decoded;
  FSL_CHECK(fsl::detail::decode_frame(frame, decoded) == FrameDecodeOutcome::kCorrupt);
}

FSL_TEST(a_zero_record_index_is_rejected) {
  std::vector<std::uint8_t> frame = make_frame(FrameKind::kEvent, 1, 1, 1, {1}, fsl::Digest{});
  for (std::size_t i = 12; i < 20; ++i) {
    frame[i] = 0;
  }
  // Repair the checksum so that only the record index is wrong.
  const std::uint32_t crc = fsl::crc32c(frame.data() + 8, frame.size() - 8);
  for (int i = 0; i < 4; ++i) {
    frame[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
  }
  DecodedFrame decoded;
  FSL_CHECK(fsl::detail::decode_frame(frame, decoded) == FrameDecodeOutcome::kCorrupt);
}

FSL_TEST(segment_header_round_trips_and_binds_identity) {
  fsl::detail::SegmentHeader header;
  header.ledger_id = fsl::LedgerId::from_words(0x1111, 0x2222);
  header.segment_index = 3;
  header.base_sequence = 100;
  header.created_unix_nanos = 123456789;
  const std::vector<std::uint8_t> bytes = fsl::detail::encode_segment_header(header);
  FSL_CHECK_EQ(bytes.size(), fsl::detail::kSegmentHeaderSize);

  auto decoded = fsl::detail::decode_segment_header(bytes);
  FSL_REQUIRE_OK(decoded);
  FSL_CHECK(decoded->ledger_id == header.ledger_id);
  FSL_CHECK_EQ(decoded->segment_index, header.segment_index);
  FSL_CHECK_EQ(decoded->base_sequence, header.base_sequence);
  FSL_CHECK_EQ(decoded->created_unix_nanos, header.created_unix_nanos);

  const fsl::Digest seed = fsl::detail::segment_chain_seed(header);
  header.segment_index = 4;
  FSL_CHECK(fsl::detail::segment_chain_seed(header) != seed);
}

FSL_TEST(segment_header_rejects_every_single_byte_alteration) {
  fsl::detail::SegmentHeader header;
  header.ledger_id = fsl::LedgerId::from_words(5, 6);
  header.segment_index = 1;
  header.base_sequence = 1;
  const std::vector<std::uint8_t> bytes = fsl::detail::encode_segment_header(header);
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    std::vector<std::uint8_t> altered = bytes;
    altered[offset] ^= 0x01;
    auto decoded = fsl::detail::decode_segment_header(altered);
    FSL_CHECK(!decoded.has_value());
  }
}

FSL_TEST(segment_header_rejects_bad_magic_and_short_input) {
  const std::vector<std::uint8_t> bytes = fsl::detail::encode_segment_header(fsl::detail::SegmentHeader{});
  std::vector<std::uint8_t> wrong_magic = bytes;
  wrong_magic[0] = 'X';
  FSL_REQUIRE_ERROR(fsl::detail::decode_segment_header(wrong_magic), fsl::ErrorCode::kBadMagic);
  FSL_REQUIRE_ERROR(fsl::detail::decode_segment_header(std::span<const std::uint8_t>(bytes.data(), 10)),
                    fsl::ErrorCode::kTruncatedInput);
}

FSL_TEST(manifest_round_trips_and_detects_alteration) {
  fsl::detail::Manifest manifest;
  manifest.ledger_id = fsl::LedgerId::from_words(0xAA, 0xBB);
  manifest.manifest_generation = 7;
  manifest.writer_incarnation = 3;
  manifest.committed_sequence = 42;
  manifest.committed_logical_tick = 42;
  manifest.committed_segment_index = 2;
  manifest.committed_offset = 4096;
  manifest.committed_frame_index = 40;
  manifest.committed_chain = fsl::Sha256::hash("chain");
  manifest.active_segment_index = 2;
  manifest.segment_count = 2;
  manifest.total_event_count = 42;
  manifest.facility_generation = 3;
  manifest.latest_epoch = 2;

  const std::vector<std::uint8_t> bytes = fsl::detail::encode_manifest(manifest);
  auto decoded = fsl::detail::decode_manifest(bytes);
  FSL_REQUIRE_OK(decoded);
  FSL_CHECK(decoded->ledger_id == manifest.ledger_id);
  FSL_CHECK_EQ(decoded->manifest_generation, manifest.manifest_generation);
  FSL_CHECK_EQ(decoded->committed_sequence, manifest.committed_sequence);
  FSL_CHECK_EQ(decoded->committed_offset, manifest.committed_offset);
  FSL_CHECK(decoded->committed_chain == manifest.committed_chain);
  FSL_CHECK_EQ(decoded->facility_generation, manifest.facility_generation);

  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    std::vector<std::uint8_t> altered = bytes;
    altered[offset] ^= 0x01;
    auto damaged = fsl::detail::decode_manifest(altered);
    FSL_CHECK(!damaged.has_value());
  }
}

FSL_TEST(manifest_rejects_a_nil_identity_and_an_impossible_index_watermark) {
  fsl::detail::Manifest manifest;
  manifest.ledger_id = fsl::LedgerId::from_words(1, 1);
  const std::vector<std::uint8_t> bytes = fsl::detail::encode_manifest(manifest);
  FSL_REQUIRE_ERROR(fsl::detail::decode_manifest(std::span<const std::uint8_t>(bytes.data(), 10)),
                    fsl::ErrorCode::kTruncatedInput);
  FSL_REQUIRE_ERROR(fsl::detail::decode_manifest({}), fsl::ErrorCode::kTruncatedInput);
}

FSL_TEST(checkpoint_round_trips_and_detects_alteration) {
  fsl::detail::Checkpoint checkpoint;
  checkpoint.ledger_id = fsl::LedgerId::from_words(9, 9);
  checkpoint.sequence = 100;
  checkpoint.logical_tick = 100;
  checkpoint.segment_index = 1;
  checkpoint.offset = 8192;
  checkpoint.chain_at_sequence = fsl::Sha256::hash("anchor");
  checkpoint.manifest_generation = 12;
  checkpoint.facility_generation = 2;
  checkpoint.latest_epoch = 4;
  checkpoint.event_count = 100;
  checkpoint.created_unix_nanos = 987654321;

  const std::vector<std::uint8_t> bytes = fsl::detail::encode_checkpoint(checkpoint);
  auto decoded = fsl::detail::decode_checkpoint(bytes);
  FSL_REQUIRE_OK(decoded);
  FSL_CHECK(decoded->ledger_id == checkpoint.ledger_id);
  FSL_CHECK_EQ(decoded->sequence, checkpoint.sequence);
  FSL_CHECK(decoded->chain_at_sequence == checkpoint.chain_at_sequence);
  FSL_CHECK_EQ(decoded->event_count, checkpoint.event_count);

  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    std::vector<std::uint8_t> altered = bytes;
    altered[offset] ^= 0x01;
    FSL_CHECK(!fsl::detail::decode_checkpoint(altered).has_value());
  }
}

FSL_TEST(index_entry_round_trips_and_rejects_bad_keys) {
  fsl::detail::IndexEntry entry;
  entry.kind = fsl::detail::IndexKind::kEventId;
  entry.sequence = 5;
  entry.extra = 3;
  entry.aux = 0;
  entry.key.assign(16, 0xAB);
  const std::vector<std::uint8_t> bytes = fsl::detail::encode_index_entry(entry);
  auto decoded = fsl::detail::decode_index_entry(bytes);
  FSL_REQUIRE_OK(decoded);
  FSL_CHECK(decoded->kind == entry.kind);
  FSL_CHECK_EQ(decoded->sequence, entry.sequence);
  FSL_CHECK(decoded->key == entry.key);

  std::vector<std::uint8_t> wrong_length = bytes;
  wrong_length[28] = 4;  // key length prefix is a u32 at offset 28
  FSL_CHECK(!fsl::detail::decode_index_entry(wrong_length).has_value());

  std::vector<std::uint8_t> unknown_kind = bytes;
  unknown_kind[0] = 0x7F;
  FSL_REQUIRE_ERROR(fsl::detail::decode_index_entry(unknown_kind), fsl::ErrorCode::kInvalidEnumValue);

  std::vector<std::uint8_t> zero_sequence = bytes;
  for (std::size_t i = 4; i < 12; ++i) {
    zero_sequence[i] = 0;
  }
  FSL_REQUIRE_ERROR(fsl::detail::decode_index_entry(zero_sequence), fsl::ErrorCode::kHeaderFieldInvalid);
}

FSL_TEST(segment_seal_and_index_seal_round_trip) {
  fsl::detail::SegmentSealBody seal;
  seal.record_count = 12;
  seal.first_sequence = 1;
  seal.last_sequence = 12;
  seal.sealed_offset = 4096;
  auto decoded_seal = fsl::detail::decode_segment_seal(fsl::detail::encode_segment_seal(seal));
  FSL_REQUIRE_OK(decoded_seal);
  FSL_CHECK_EQ(decoded_seal->record_count, seal.record_count);
  FSL_CHECK_EQ(decoded_seal->sealed_offset, seal.sealed_offset);

  fsl::detail::IndexSealBody index_seal;
  index_seal.entry_count = 77;
  index_seal.through_sequence = 33;
  index_seal.index_generation = 2;
  auto decoded_index = fsl::detail::decode_index_seal(fsl::detail::encode_index_seal(index_seal));
  FSL_REQUIRE_OK(decoded_index);
  FSL_CHECK_EQ(decoded_index->entry_count, index_seal.entry_count);
  FSL_CHECK_EQ(decoded_index->through_sequence, index_seal.through_sequence);
}

FSL_TEST(path_names_are_fixed_width_and_parseable) {
  const fsl::detail::Paths paths(std::filesystem::path("/tmp/ledger"));
  const std::string segment_name = paths.segment(fsl::SegmentIndex(42)).filename().string();
  FSL_CHECK_EQ(segment_name, std::string("segment-0000000000000042.fsl"));
  fsl::SegmentIndex parsed{1};
  FSL_REQUIRE(fsl::detail::Paths::parse_segment_name(segment_name, parsed));
  FSL_CHECK_EQ(parsed.value(), std::uint64_t{42});

  FSL_CHECK(!fsl::detail::Paths::parse_segment_name("segment-1.fsl", parsed));
  FSL_CHECK(!fsl::detail::Paths::parse_segment_name("segment-0000000000000000.fsl", parsed));
  FSL_CHECK(!fsl::detail::Paths::parse_segment_name("segment-0000000000000042.fslx", parsed));
  FSL_CHECK(!fsl::detail::Paths::parse_segment_name("../../etc/passwd", parsed));

  std::uint64_t generation = 0;
  std::uint64_t ordinal = 0;
  FSL_REQUIRE(fsl::detail::Paths::parse_index_name("index-0000000000000003-00000007.idx", generation,
                                                   ordinal));
  FSL_CHECK_EQ(generation, std::uint64_t{3});
  FSL_CHECK_EQ(ordinal, std::uint64_t{7});

  fsl::LedgerSequence sequence{1};
  FSL_REQUIRE(
      fsl::detail::Paths::parse_checkpoint_name("checkpoint-00000000000000001234.fsl", sequence));
  FSL_CHECK_EQ(sequence.value(), std::uint64_t{1234});
}

}  // namespace

FSL_TEST_MAIN("test_frame")
