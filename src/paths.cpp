// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "detail/paths.hpp"

#include <array>

namespace fsl::detail {
namespace {

constexpr std::string_view kSegmentPrefix = "segment-";
constexpr std::string_view kSegmentSuffix = ".fsl";
constexpr std::string_view kIndexPrefix = "index-";
constexpr std::string_view kIndexSuffix = ".idx";
constexpr std::string_view kCheckpointPrefix = "checkpoint-";
constexpr std::string_view kCheckpointSuffix = ".fsl";
constexpr std::size_t kSegmentDigits = 16;
constexpr std::size_t kCheckpointDigits = 20;

[[nodiscard]] std::string fixed_width(std::uint64_t value, std::size_t digits) {
  std::string text(digits, '0');
  for (std::size_t i = 0; i < digits; ++i) {
    text[digits - 1 - i] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  return text;
}

/// Parses exactly `digits` decimal digits, rejecting anything else.
[[nodiscard]] bool parse_fixed_width(std::string_view text, std::size_t digits, std::uint64_t& out) noexcept {
  if (text.size() != digits) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

[[nodiscard]] bool has_prefix_and_suffix(std::string_view name,
                                         std::string_view prefix,
                                         std::string_view suffix) noexcept {
  if (name.size() != prefix.size() + suffix.size() + 1 && name.size() < prefix.size() + suffix.size()) {
    return false;
  }
  if (name.size() <= prefix.size() + suffix.size()) {
    return false;
  }
  return name.substr(0, prefix.size()) == prefix &&
         name.substr(name.size() - suffix.size()) == suffix;
}

}  // namespace

std::filesystem::path Paths::segment(SegmentIndex index) const {
  return segments_directory() / (std::string(kSegmentPrefix) + fixed_width(index.value(), kSegmentDigits) +
                                 std::string(kSegmentSuffix));
}

std::filesystem::path Paths::index_file(std::uint64_t generation, std::uint64_t ordinal) const {
  return index_directory() / (std::string(kIndexPrefix) + fixed_width(generation, kSegmentDigits) + "-" +
                              fixed_width(ordinal, 8) + std::string(kIndexSuffix));
}

std::filesystem::path Paths::checkpoint(LedgerSequence sequence) const {
  return checkpoints_directory() / (std::string(kCheckpointPrefix) +
                                    fixed_width(sequence.value(), kCheckpointDigits) +
                                    std::string(kCheckpointSuffix));
}

bool Paths::parse_segment_name(std::string_view name, SegmentIndex& out) noexcept {
  if (!has_prefix_and_suffix(name, kSegmentPrefix, kSegmentSuffix)) {
    return false;
  }
  const std::string_view digits =
      name.substr(kSegmentPrefix.size(), name.size() - kSegmentPrefix.size() - kSegmentSuffix.size());
  std::uint64_t value = 0;
  if (!parse_fixed_width(digits, kSegmentDigits, value) || value == 0) {
    return false;
  }
  out = SegmentIndex(value);
  return true;
}

bool Paths::parse_index_name(std::string_view name, std::uint64_t& generation, std::uint64_t& ordinal) noexcept {
  if (!has_prefix_and_suffix(name, kIndexPrefix, kIndexSuffix)) {
    return false;
  }
  const std::string_view body =
      name.substr(kIndexPrefix.size(), name.size() - kIndexPrefix.size() - kIndexSuffix.size());
  if (body.size() != kSegmentDigits + 1 + 8 || body[kSegmentDigits] != '-') {
    return false;
  }
  return parse_fixed_width(body.substr(0, kSegmentDigits), kSegmentDigits, generation) &&
         parse_fixed_width(body.substr(kSegmentDigits + 1), 8, ordinal);
}

bool Paths::parse_checkpoint_name(std::string_view name, LedgerSequence& out) noexcept {
  if (!has_prefix_and_suffix(name, kCheckpointPrefix, kCheckpointSuffix)) {
    return false;
  }
  const std::string_view digits = name.substr(kCheckpointPrefix.size(),
                                              name.size() - kCheckpointPrefix.size() -
                                                  kCheckpointSuffix.size());
  std::uint64_t value = 0;
  if (!parse_fixed_width(digits, kCheckpointDigits, value) || value == 0) {
    return false;
  }
  out = LedgerSequence(value);
  return true;
}

bool Paths::is_managed_name(std::string_view name) noexcept {
  if (name == "ledger.manifest" || name == "ledger.manifest.bak" || name == "ledger.manifest.tmp" ||
      name == "ledger.lock") {
    return true;
  }
  SegmentIndex segment_index{1};
  if (parse_segment_name(name, segment_index)) {
    return true;
  }
  std::uint64_t generation = 0;
  std::uint64_t ordinal = 0;
  if (parse_index_name(name, generation, ordinal)) {
    return true;
  }
  LedgerSequence sequence{1};
  return parse_checkpoint_name(name, sequence);
}

}  // namespace fsl::detail
