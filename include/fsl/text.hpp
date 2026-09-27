// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// \file text.hpp
/// Text and byte-string validation used at every trust boundary.
///
/// Every function here is a pure predicate or a pure transformation over
/// caller-owned buffers. Nothing allocates unless the caller asks for the
/// materialised result, and nothing normalises silently: inputs that are not in
/// the canonical form this library documents are rejected, not rewritten.

namespace fsl {

/// Lowercase hexadecimal rendering of arbitrary bytes.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
/// Lowercase hexadecimal rendering of arbitrary bytes.
[[nodiscard]] std::string to_hex(std::string_view bytes);

/// Decodes hexadecimal text. Accepts upper and lower case letters; rejects odd
/// lengths, non-hex characters, and inputs larger than `max_bytes`.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text,
                                                                std::size_t max_bytes = 4096);
/// Decodes fixed-width hexadecimal text into `out` (exactly `out.size()` bytes).
[[nodiscard]] bool from_hex_into(std::string_view text, std::span<std::uint8_t> out) noexcept;

/// True when every character is a hexadecimal digit (either case).
[[nodiscard]] bool is_hexadecimal(std::string_view text) noexcept;

/// Strict UTF-8 validation: rejects overlong encodings, surrogate code points
/// (U+D800..U+DFFF), code points above U+10FFFF, and truncated sequences.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// True when the string contains no ASCII control character (0x00..0x1F, 0x7F).
[[nodiscard]] bool has_no_control_characters(std::string_view text) noexcept;

/// Canonical identifier syntax: first character an ASCII lowercase letter or
/// digit, remaining characters from `[a-z0-9._-]`, total length in
/// `[1, max_length]`. Uppercase is rejected rather than folded.
[[nodiscard]] bool is_valid_identifier(std::string_view text, std::size_t max_length) noexcept;

/// Canonical subject key syntax: first and last character an ASCII lowercase
/// letter or digit, interior characters from `[a-z0-9._:/-]`, total length in
/// `[1, max_length]`. `..` is rejected so that a key can never be read as a
/// relative path traversal, and `\` is never accepted.
[[nodiscard]] bool is_valid_subject_key(std::string_view text, std::size_t max_length) noexcept;

}  // namespace fsl
