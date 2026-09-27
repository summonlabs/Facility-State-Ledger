// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

/// \file version.hpp
/// Compile-time version identification for the Facility State Ledger library.
///
/// The library version identifies the public API and the on-disk segment format
/// generation. Persisted compatibility is governed by kSegmentFormatVersion in
/// fsl/record.hpp, which is versioned independently of the library version.

#define FSL_VERSION_MAJOR 1
#define FSL_VERSION_MINOR 0
#define FSL_VERSION_PATCH 0

#define FSL_VERSION_STRING "1.0.0"

namespace fsl {

/// Semantic version of the public library surface.
struct Version {
  std::uint32_t major = FSL_VERSION_MAJOR;
  std::uint32_t minor = FSL_VERSION_MINOR;
  std::uint32_t patch = FSL_VERSION_PATCH;

  friend constexpr bool operator==(const Version&, const Version&) = default;
};

/// Version of the running library.
[[nodiscard]] constexpr Version library_version() noexcept {
  return Version{FSL_VERSION_MAJOR, FSL_VERSION_MINOR, FSL_VERSION_PATCH};
}

/// "1.0.0" - stable textual form used by tooling and packaging.
[[nodiscard]] constexpr std::string_view library_version_string() noexcept {
  return FSL_VERSION_STRING;
}

}  // namespace fsl
