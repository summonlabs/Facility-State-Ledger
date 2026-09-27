// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/subject.hpp"

#include <array>

#include "fsl/text.hpp"

namespace fsl {
namespace {

struct SubjectKindName {
  SubjectKind kind;
  std::string_view name;
};

constexpr std::array<SubjectKindName, 9> kSubjectKindNames{{
    {SubjectKind::kRack, "rack"},
    {SubjectKind::kAsset, "asset"},
    {SubjectKind::kLocation, "location"},
    {SubjectKind::kFacilityNode, "facility-node"},
    {SubjectKind::kPowerDomain, "power-domain"},
    {SubjectKind::kCoolingDomain, "cooling-domain"},
    {SubjectKind::kExternalAsi, "external-asi"},
    {SubjectKind::kExternalDfi, "external-dfi"},
    {SubjectKind::kLedger, "ledger"},
}};

struct RelationshipKindName {
  RelationshipKind kind;
  std::string_view name;
};

constexpr std::array<RelationshipKindName, 5> kRelationshipKindNames{{
    {RelationshipKind::kContainedIn, "contained-in"},
    {RelationshipKind::kPoweredBy, "powered-by"},
    {RelationshipKind::kCooledBy, "cooled-by"},
    {RelationshipKind::kConnectedTo, "connected-to"},
    {RelationshipKind::kDependsOn, "depends-on"},
}};

}  // namespace

std::string_view to_string(SubjectKind kind) noexcept {
  for (const auto& entry : kSubjectKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

std::optional<SubjectKind> subject_kind_from_string(std::string_view name) noexcept {
  for (const auto& entry : kSubjectKindNames) {
    if (entry.name == name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

bool subject_kind_requires_location(SubjectKind kind) noexcept {
  switch (kind) {
    case SubjectKind::kRack:
    case SubjectKind::kAsset:
    case SubjectKind::kFacilityNode:
      return true;
    case SubjectKind::kLocation:
    case SubjectKind::kPowerDomain:
    case SubjectKind::kCoolingDomain:
    case SubjectKind::kExternalAsi:
    case SubjectKind::kExternalDfi:
    case SubjectKind::kLedger:
      return false;
  }
  return false;
}

std::string_view to_string(RelationshipKind kind) noexcept {
  for (const auto& entry : kRelationshipKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

std::optional<RelationshipKind> relationship_kind_from_string(std::string_view name) noexcept {
  for (const auto& entry : kRelationshipKindNames) {
    if (entry.name == name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

Result<SubjectKey> SubjectKey::parse(std::string_view text) {
  if (text.empty()) {
    return invalid_argument(ErrorCode::kValueEmpty, "subject key is empty");
  }
  if (text.size() > kMaxLength) {
    return invalid_argument(ErrorCode::kValueTooLong,
                            "subject key is longer than " + std::to_string(kMaxLength) + " characters");
  }
  if (!has_no_control_characters(text)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid, "subject key contains a control character");
  }
  if (!is_valid_subject_key(text, kMaxLength)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            "subject key must be lower-case [a-z0-9._:/-], start and end with a letter or "
                            "digit, and must not contain '..': '" +
                                std::string(text) + "'");
  }
  return SubjectKey(std::string(text));
}

bool SubjectKey::is_valid(std::string_view text) noexcept {
  return has_no_control_characters(text) && is_valid_subject_key(text, kMaxLength);
}

Result<SubjectRef> SubjectRef::create(SubjectKind kind, std::string_view key) {
  auto parsed = SubjectKey::parse(key);
  if (!parsed.has_value()) {
    return parsed.status();
  }
  return SubjectRef(kind, std::move(parsed).value());
}

Result<SubjectRef> SubjectRef::parse(std::string_view text) {
  const std::size_t separator = text.find('/');
  if (separator == std::string_view::npos || separator == 0) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            "subject reference must be '<kind>/<key>': '" + std::string(text) + "'");
  }
  const std::string_view kind_text = text.substr(0, separator);
  const std::string_view key_text = text.substr(separator + 1);
  const auto kind = subject_kind_from_string(kind_text);
  if (!kind.has_value()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue,
                            "unknown subject kind '" + std::string(kind_text) + "'");
  }
  return create(*kind, key_text);
}

std::string SubjectRef::to_string() const {
  std::string result;
  result.reserve(key_.view().size() + 16);
  result.append(fsl::to_string(kind_));
  result.push_back('/');
  result.append(key_.view());
  return result;
}

}  // namespace fsl
