// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "fsl/counter.hpp"

/// \file paths.hpp
/// The on-disk layout of a ledger directory.
///
///   <root>/ledger.manifest        authoritative commit watermark
///   <root>/ledger.manifest.bak    previous publication, for fallback
///   <root>/ledger.manifest.tmp    staging file for the next publication
///   <root>/ledger.lock            writer lock
///   <root>/segments/segment-<16 hex digits>.fsl
///   <root>/index/index-<generation>-<ordinal>.idx
///   <root>/checkpoints/checkpoint-<20 digits>.fsl
///
/// Every name is fixed-width and lower-case so that the lexicographic order of
/// the directory listing is the numeric order of the objects. No name is ever
/// derived from untrusted input: segment, index and checkpoint names come from
/// counters the ledger itself assigns.

namespace fsl::detail {

class Paths {
 public:
  explicit Paths(std::filesystem::path root) : root_(std::move(root)) {}

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] std::filesystem::path manifest() const { return root_ / "ledger.manifest"; }
  [[nodiscard]] std::filesystem::path manifest_backup() const { return root_ / "ledger.manifest.bak"; }
  [[nodiscard]] std::filesystem::path manifest_temp() const { return root_ / "ledger.manifest.tmp"; }
  [[nodiscard]] std::filesystem::path lock_file() const { return root_ / "ledger.lock"; }
  [[nodiscard]] std::filesystem::path segments_directory() const { return root_ / "segments"; }
  [[nodiscard]] std::filesystem::path index_directory() const { return root_ / "index"; }
  [[nodiscard]] std::filesystem::path checkpoints_directory() const { return root_ / "checkpoints"; }

  [[nodiscard]] std::filesystem::path segment(SegmentIndex index) const;
  [[nodiscard]] std::filesystem::path index_file(std::uint64_t generation, std::uint64_t ordinal) const;
  [[nodiscard]] std::filesystem::path checkpoint(LedgerSequence sequence) const;

  /// Parses a segment file name. Returns false for anything that is not exactly
  /// the canonical form this library writes.
  [[nodiscard]] static bool parse_segment_name(std::string_view name, SegmentIndex& out) noexcept;
  [[nodiscard]] static bool parse_index_name(std::string_view name,
                                             std::uint64_t& generation,
                                             std::uint64_t& ordinal) noexcept;
  [[nodiscard]] static bool parse_checkpoint_name(std::string_view name, LedgerSequence& out) noexcept;

  /// True when the file name is one this library writes into the ledger root,
  /// the segments directory, the index directory or the checkpoints directory.
  [[nodiscard]] static bool is_managed_name(std::string_view name) noexcept;

 private:
  std::filesystem::path root_;
};

}  // namespace fsl::detail
