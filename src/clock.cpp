// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/clock.hpp"

#include "detail/platform.hpp"

namespace fsl {

std::uint64_t SystemClock::monotonic_nanoseconds() noexcept {
  return detail::monotonic_nanoseconds();
}

std::uint64_t SystemClock::unix_nanoseconds() noexcept { return detail::unix_nanoseconds(); }

ManualClock::ManualClock(std::uint64_t monotonic_nanoseconds, std::uint64_t unix_nanoseconds) noexcept
    : monotonic_(monotonic_nanoseconds), unix_(unix_nanoseconds) {}

std::uint64_t ManualClock::monotonic_nanoseconds() noexcept {
  return monotonic_.load(std::memory_order_relaxed);
}

std::uint64_t ManualClock::unix_nanoseconds() noexcept { return unix_.load(std::memory_order_relaxed); }

void ManualClock::advance(std::uint64_t delta) noexcept {
  monotonic_.fetch_add(delta, std::memory_order_relaxed);
}

void ManualClock::set_unix_nanoseconds(std::uint64_t value) noexcept {
  unix_.store(value, std::memory_order_relaxed);
}

}  // namespace fsl
