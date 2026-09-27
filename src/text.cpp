// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/text.hpp"

#include <array>

namespace fsl {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] constexpr bool is_ascii_lower_alnum(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

[[nodiscard]] constexpr bool is_ascii_identifier_char(char c) noexcept {
  return is_ascii_lower_alnum(c) || c == '.' || c == '_' || c == '-';
}

[[nodiscard]] constexpr bool is_ascii_subject_char(char c) noexcept {
  return is_ascii_lower_alnum(c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
}

}  // namespace

std::string to_hex(std::span<const std::uint8_t> bytes) {
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    result.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
    result.push_back(kHexDigits[byte & 0x0Fu]);
  }
  return result;
}

std::string to_hex(std::string_view bytes) {
  return to_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

bool from_hex_into(std::string_view text, std::span<std::uint8_t> out) noexcept {
  if (text.size() != out.size() * 2) {
    return false;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int high = hex_value(text[i * 2]);
    const int low = hex_value(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t max_bytes) {
  if (text.size() % 2 != 0) {
    return std::nullopt;
  }
  const std::size_t byte_count = text.size() / 2;
  if (byte_count > max_bytes) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> out(byte_count);
  if (!from_hex_into(text, out)) {
    return std::nullopt;
  }
  return out;
}

bool is_hexadecimal(std::string_view text) noexcept {
  for (const char c : text) {
    if (hex_value(c) < 0) {
      return false;
    }
  }
  return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
  const std::size_t size = text.size();
  std::size_t i = 0;
  while (i < size) {
    const std::uint8_t lead = data[i];
    if (lead < 0x80) {
      ++i;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0u) == 0xC0u) {
      continuation = 1;
      code_point = lead & 0x1Fu;
      minimum = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      continuation = 2;
      code_point = lead & 0x0Fu;
      minimum = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      continuation = 3;
      code_point = lead & 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }
    if (i + continuation >= size) {
      return false;
    }
    for (std::size_t k = 1; k <= continuation; ++k) {
      const std::uint8_t next = data[i + k];
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (code_point < minimum) {
      return false;  // overlong encoding
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;  // UTF-16 surrogate half
    }
    i += continuation + 1;
  }
  return true;
}

bool has_no_control_characters(std::string_view text) noexcept {
  for (const char c : text) {
    const auto value = static_cast<unsigned char>(c);
    if (value < 0x20u || value == 0x7Fu) {
      return false;
    }
  }
  return true;
}

bool is_valid_identifier(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  if (!is_ascii_lower_alnum(text.front())) {
    return false;
  }
  for (const char c : text) {
    if (!is_ascii_identifier_char(c)) {
      return false;
    }
  }
  return true;
}

bool is_valid_subject_key(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  if (!is_ascii_lower_alnum(text.front()) || !is_ascii_lower_alnum(text.back())) {
    return false;
  }
  for (const char c : text) {
    if (!is_ascii_subject_char(c)) {
      return false;
    }
  }
  // A key can never be read as a relative path traversal.
  return text.find("..") == std::string_view::npos;
}

}  // namespace fsl
