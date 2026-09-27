// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/options.hpp"

#include "fsl/fault.hpp"
#include "fsl/query.hpp"

namespace fsl {

std::string_view to_string(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::kReadWrite:
      return "read-write";
    case OpenMode::kReadOnly:
      return "read-only";
    case OpenMode::kCreate:
      return "create";
    case OpenMode::kDiagnose:
      return "diagnose";
  }
  return "unknown";
}

std::string_view to_string(RecoveryPolicy policy) noexcept {
  switch (policy) {
    case RecoveryPolicy::kTruncateUncommittedTail:
      return "truncate-uncommitted-tail";
    case RecoveryPolicy::kRefuseOnUncommittedTail:
      return "refuse-on-uncommitted-tail";
  }
  return "unknown";
}

std::string_view to_string(DuplicateDetection mode) noexcept {
  switch (mode) {
    case DuplicateDetection::kExact:
      return "exact";
    case DuplicateDetection::kWindowed:
      return "windowed";
  }
  return "unknown";
}

std::string_view to_string(ReaderLockPolicy policy) noexcept {
  switch (policy) {
    case ReaderLockPolicy::kNone:
      return "none";
    case ReaderLockPolicy::kShared:
      return "shared";
  }
  return "unknown";
}

std::string_view to_string(QuerySource source) noexcept {
  switch (source) {
    case QuerySource::kIndexed:
      return "indexed";
    case QuerySource::kLinearScan:
      return "linear-scan";
  }
  return "unknown";
}

std::string_view to_string(VerifyScope scope) noexcept {
  switch (scope) {
    case VerifyScope::kManifestTail:
      return "manifest-tail";
    case VerifyScope::kFull:
      return "full";
    case VerifyScope::kFromCheckpoint:
      return "from-checkpoint";
  }
  return "unknown";
}

std::string_view to_string(Boundary boundary) noexcept {
  switch (boundary) {
    case Boundary::kAfterEncode:
      return "after-encode";
    case Boundary::kAfterSegmentWrite:
      return "after-segment-write";
    case Boundary::kAfterSegmentFlush:
      return "after-segment-flush";
    case Boundary::kBeforeManifestPublish:
      return "before-manifest-publish";
    case Boundary::kAfterManifestPublish:
      return "after-manifest-publish";
    case Boundary::kAfterCommitAck:
      return "after-commit-ack";
    case Boundary::kAfterRotateSealWrite:
      return "after-rotate-seal-write";
    case Boundary::kAfterRecoveryScan:
      return "after-recovery-scan";
  }
  return "unknown";
}

}  // namespace fsl
