// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/ids.hpp"

#include "detail/platform.hpp"
#include "fsl/text.hpp"

namespace fsl {

Result<LedgerId> generate_ledger_id() {
  LedgerId id;
  std::span<std::uint8_t> target(const_cast<std::uint8_t*>(id.data()), LedgerId::kByteSize);
  if (Result<void> filled = detail::random_bytes(target); filled.has_value() == false) {
    return filled.status();
  }
  // A nil identity is never assigned; the entropy source would have to fail
  // catastrophically for this to trigger, and the check keeps the contract
  // "LedgerId::is_nil() is always false for an open ledger" unconditional.
  if (id.is_nil()) {
    return internal_error(ErrorCode::kInternalError,
                          "the entropy source produced a nil ledger identity");
  }
  return id;
}

Result<SourceComponentId> SourceComponentId::parse(std::string_view text) {
  if (text.empty()) {
    return invalid_argument(ErrorCode::kValueEmpty, "source component identifier is empty");
  }
  if (text.size() > kMaxLength) {
    return invalid_argument(ErrorCode::kValueTooLong,
                            "source component identifier is longer than " + std::to_string(kMaxLength) +
                                " characters");
  }
  if (!is_valid_identifier(text, kMaxLength)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            "source component identifier must be lower-case [a-z0-9._-] and start with a "
                            "letter or digit: '" +
                                std::string(text) + "'");
  }
  return SourceComponentId(std::string(text));
}

bool SourceComponentId::is_valid(std::string_view text) noexcept {
  return is_valid_identifier(text, kMaxLength);
}

Result<SchemaId> SchemaId::parse(std::string_view text) {
  if (text.empty()) {
    return invalid_argument(ErrorCode::kValueEmpty, "payload schema identifier is empty");
  }
  if (text.size() > kMaxLength) {
    return invalid_argument(ErrorCode::kValueTooLong,
                            "payload schema identifier is longer than " + std::to_string(kMaxLength) +
                                " characters");
  }
  if (!is_valid_identifier(text, kMaxLength)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            "payload schema identifier must be lower-case [a-z0-9._-] and start with a "
                            "letter or digit: '" +
                                std::string(text) + "'");
  }
  return SchemaId(std::string(text));
}

bool SchemaId::is_valid(std::string_view text) noexcept { return is_valid_identifier(text, kMaxLength); }

}  // namespace fsl
