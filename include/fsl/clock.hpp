// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

/// \file clock.hpp
/// Injectable time sources.
///
/// The ledger never derives ordering from a clock. A monotonic reading is
/// recorded with each accepted event for operator context, and a wall-clock
/// reading is recorded once in each segment header. Both are informational: the
/// only ordering this library guarantees is ascending LedgerSequence. A
/// deterministic clock can therefore be supplied without weakening any
/// invariant.

namespace fsl {

/// Source of monotonic and wall-clock readings.
///
/// Implementations must be safe to call concurrently, must never throw, and must
/// not re-enter the ledger.
class IClock {
 public:
  IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  IClock(IClock&&) = delete;
  IClock& operator=(IClock&&) = delete;
  virtual ~IClock() = default;

  /// Nanoseconds from an unspecified monotonic origin. Non-decreasing within one
  /// process incarnation; not comparable across restarts or processes.
  [[nodiscard]] virtual std::uint64_t monotonic_nanoseconds() noexcept = 0;

  /// Nanoseconds since the Unix epoch, or 0 when unavailable. Informational.
  [[nodiscard]] virtual std::uint64_t unix_nanoseconds() noexcept = 0;
};

/// Reads the operating system monotonic and system clocks.
class SystemClock final : public IClock {
 public:
  SystemClock() = default;

  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override;
  [[nodiscard]] std::uint64_t unix_nanoseconds() noexcept override;
};

/// Deterministic clock for tests and reproducible pipelines.
class ManualClock final : public IClock {
 public:
  explicit ManualClock(std::uint64_t monotonic_nanoseconds = 0, std::uint64_t unix_nanoseconds = 0) noexcept;

  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override;
  [[nodiscard]] std::uint64_t unix_nanoseconds() noexcept override;

  /// Advances the monotonic reading by `delta`. A zero delta is a no-op.
  void advance(std::uint64_t delta) noexcept;
  /// Sets the wall-clock reading. Never affects the monotonic reading.
  void set_unix_nanoseconds(std::uint64_t value) noexcept;

 private:
  std::atomic<std::uint64_t> monotonic_;
  std::atomic<std::uint64_t> unix_;
};

}  // namespace fsl
