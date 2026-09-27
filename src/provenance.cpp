// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/provenance.hpp"

#include <algorithm>

#include "fsl/text.hpp"

namespace fsl {

Result<ProvenanceRecord> ProvenanceRecord::create(SourceComponentId source,
                                                  SourceGeneration source_generation,
                                                  std::optional<SourceSequence> source_sequence,
                                                  std::optional<std::uint64_t> source_wall_clock_unix_nanos,
                                                  std::vector<ProvenanceAttribute> attributes) {
  if (source_generation.is_zero()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue, "source generation zero is not a valid incarnation");
  }
  if (source_sequence.has_value() && source_sequence->is_zero()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue, "source sequence zero is not a valid position");
  }
  if (attributes.size() > kMaxAttributes) {
    return capacity_exceeded(ErrorCode::kCountOutOfRange,
                             "provenance carries " + std::to_string(attributes.size()) +
                                 " attributes, the limit is " + std::to_string(kMaxAttributes));
  }

  for (const ProvenanceAttribute& attribute : attributes) {
    if (attribute.key.empty()) {
      return invalid_argument(ErrorCode::kValueEmpty, "provenance attribute key is empty");
    }
    if (attribute.key.size() > ProvenanceAttribute::kMaxKeyLength) {
      return invalid_argument(ErrorCode::kValueTooLong,
                              "provenance attribute key '" + attribute.key + "' is longer than " +
                                  std::to_string(ProvenanceAttribute::kMaxKeyLength) + " characters");
    }
    if (!is_valid_identifier(attribute.key, ProvenanceAttribute::kMaxKeyLength)) {
      return invalid_argument(ErrorCode::kSyntaxInvalid,
                              "provenance attribute key must be lower-case [a-z0-9._-]: '" + attribute.key +
                                  "'");
    }
    if (attribute.value.size() > ProvenanceAttribute::kMaxValueLength) {
      return invalid_argument(ErrorCode::kValueTooLong,
                              "provenance attribute '" + attribute.key + "' value is longer than " +
                                  std::to_string(ProvenanceAttribute::kMaxValueLength) + " bytes");
    }
    if (!is_valid_utf8(attribute.value)) {
      return invalid_argument(ErrorCode::kInvalidUtf8,
                              "provenance attribute '" + attribute.key + "' value is not valid UTF-8");
    }
    if (!has_no_control_characters(attribute.value)) {
      return invalid_argument(ErrorCode::kSyntaxInvalid,
                              "provenance attribute '" + attribute.key + "' value contains a control "
                              "character");
    }
  }

  std::sort(attributes.begin(), attributes.end());
  for (std::size_t i = 1; i < attributes.size(); ++i) {
    if (attributes[i - 1].key == attributes[i].key) {
      return conflict(ErrorCode::kDuplicateKey,
                      "provenance attribute '" + attributes[i].key + "' is supplied more than once");
    }
  }

  return ProvenanceRecord(std::move(source),
                          source_generation,
                          source_sequence,
                          source_wall_clock_unix_nanos,
                          std::move(attributes));
}

const std::string* ProvenanceRecord::find_attribute(std::string_view key) const noexcept {
  for (const ProvenanceAttribute& attribute : attributes_) {
    if (attribute.key == key) {
      return &attribute.value;
    }
  }
  return nullptr;
}

}  // namespace fsl
