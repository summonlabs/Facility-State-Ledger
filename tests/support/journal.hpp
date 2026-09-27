// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "fsl/ledger.hpp"
#include "support/test_support.hpp"

/// \file journal.hpp
/// Helpers that drive a ledger through its ordinary lifecycle, so that suites
/// express the scenario they care about rather than the boilerplate.

namespace fsl_test {

/// Creates a ledger at `dir` with the deterministic options, or opens it when it
/// already holds one.
[[nodiscard]] fsl::Result<fsl::Ledger> open_journal(
    const std::filesystem::path& dir,
    const fsl::LedgerOptions& options = deterministic_options());

/// Opens epoch 1 in facility generation 1.
[[nodiscard]] fsl::Result<fsl::AppendOutcome> open_epoch(fsl::Ledger& ledger,
                                                         std::uint64_t epoch,
                                                         std::uint64_t generation,
                                                         std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> close_epoch(fsl::Ledger& ledger,
                                                          std::uint64_t epoch,
                                                          std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> advance_generation(fsl::Ledger& ledger,
                                                                 std::uint64_t from,
                                                                 std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> register_subject(fsl::Ledger& ledger,
                                                               const fsl::SubjectRef& subject,
                                                               const std::optional<fsl::SubjectRef>& location,
                                                               std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> retire_subject(fsl::Ledger& ledger,
                                                             const fsl::SubjectRef& subject,
                                                             const std::string& reason,
                                                             std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> record_observation(fsl::Ledger& ledger,
                                                                 const fsl::SubjectRef& subject,
                                                                 const std::string& reference,
                                                                 std::uint64_t identity_index,
                                                                 std::size_t body_bytes = 32);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> mutate_subject(fsl::Ledger& ledger,
                                                             const fsl::SubjectRef& subject,
                                                             const std::string& reference,
                                                             std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> assert_relationship(fsl::Ledger& ledger,
                                                                  const fsl::SubjectRef& subject,
                                                                  const fsl::SubjectRef& related,
                                                                  fsl::RelationshipKind kind,
                                                                  std::uint64_t identity_index);

[[nodiscard]] fsl::Result<fsl::AppendOutcome> retract_relationship(fsl::Ledger& ledger,
                                                                   const fsl::SubjectRef& subject,
                                                                   const fsl::SubjectRef& related,
                                                                   fsl::RelationshipKind kind,
                                                                   std::uint64_t identity_index);

/// Registers one location, one rack inside it and one asset inside the rack,
/// after opening epoch 1. Returns the three references.
struct FacilitySeed {
  fsl::SubjectRef location;
  fsl::SubjectRef rack;
  fsl::SubjectRef asset;
};

[[nodiscard]] fsl::Result<FacilitySeed> seed_facility(fsl::Ledger& ledger,
                                                      std::uint64_t first_identity_index = 100);

/// Reads the whole ledger through a fresh replay and returns the sequences in
/// delivery order.
[[nodiscard]] fsl::Result<std::vector<std::uint64_t>> replay_sequences(const fsl::Ledger& ledger);

}  // namespace fsl_test
