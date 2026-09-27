// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

/// \file fault.hpp
/// Crash- and failure-injection support.
///
/// This surface exists so that durability can be proven rather than asserted:
/// a test process installs an injector, appends once, and terminates abruptly at
/// a named boundary. It is not an extension point for production behaviour.
///
/// Contract for implementors:
///   * on_boundary runs on the calling thread, inside the mutation path, while
///     the ledger's internal mutation lock is held.
///   * It must therefore not call back into the same Ledger instance, must not
///     throw, and must not block on another thread that is waiting for that
///     ledger.
///   * Returning normally lets the operation continue. Terminating the process
///     (std::_Exit, TerminateProcess) is the intended use for crash proofs;
///     returning an error is not supported because the boundary is past the
///     point where the operation could be undone.

namespace fsl {

/// Named points on the durable commit path.
enum class Boundary : std::uint8_t {
  /// Event frames are encoded in memory; nothing has been written.
  kAfterEncode = 1,
  /// Frame bytes were handed to the operating system; no flush has been issued.
  kAfterSegmentWrite = 2,
  /// The segment flush completed; the manifest is not yet updated.
  kAfterSegmentFlush = 3,
  /// The replacement manifest is fully written to its temporary file but has not
  /// been published. This is the classic torn-commit window.
  kBeforeManifestPublish = 4,
  /// The manifest rename completed. The commit is durable.
  kAfterManifestPublish = 5,
  /// The append is about to return success to its caller.
  kAfterCommitAck = 6,
  /// A segment seal frame was written and flushed during rotation.
  kAfterRotateSealWrite = 7,
  /// Recovery finished scanning but has not yet acted on what it found.
  kAfterRecoveryScan = 8,
};

[[nodiscard]] std::string_view to_string(Boundary boundary) noexcept;

/// Receives control at named durability boundaries.
class IFaultInjector {
 public:
  IFaultInjector() = default;
  IFaultInjector(const IFaultInjector&) = delete;
  IFaultInjector& operator=(const IFaultInjector&) = delete;
  IFaultInjector(IFaultInjector&&) = delete;
  IFaultInjector& operator=(IFaultInjector&&) = delete;
  virtual ~IFaultInjector() = default;

  virtual void on_boundary(Boundary boundary) = 0;
};

}  // namespace fsl
