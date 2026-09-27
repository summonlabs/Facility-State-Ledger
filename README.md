# Facility State Ledger

**DCCP Tranche 1 — Canonical Facility State, repository 7 of 8.**

A durable, provenance-preserving ledger of authoritative facility-state
transitions: accepted observations, mutations, reconciliation events and
generation changes. It is the append-oriented historical record that higher DCCP
control layers read to learn *what the facility was*, *which generation was
authoritative*, and *who said so*.

Apache License 2.0. C++20. Standard library only.

---

## 1. Systems boundary

**This repository owns** the durable, provenance-preserving record of
authoritative facility-state transitions, and the guarantees that make that
record trustworthy:

* an append-only segment log with a versioned, integrity-checked frame format;
* a commit watermark that defines exactly which records are authoritative;
* deterministic ordering by ledger sequence, and deterministic replay;
* admission policy for what may be appended, keyed on epoch, generation, subject
  lifecycle and producer incarnation;
* idempotent duplicate semantics for retried submissions;
* crash recovery that preserves every committed record and discards only
  unacknowledged tail state;
* audit export whose bytes are a function of ledger content alone;
* a derived index that can be rebuilt from the log at any time.

**This repository does not own** facility capacity planning, placement,
reservations, electrical actuation, cooling control, maintenance orchestration,
tenancy policy, incident response, observability dashboards, or multi-site
federation. Objects owned by Accelerated Systems Infrastructure (ASI) and
Distributed Fabric Infrastructure (DFI) are referenced through typed opaque
identifiers (`fsl::SubjectKind::kExternalAsi`, `kExternalDfi`); their semantics
are never reimplemented here.

The ledger records that a state transition was *submitted and accepted*. It does
not decide whether the domain-specific content of that transition is correct —
a rack's rated capacity belongs to the component that owns it. What the ledger
*does* enforce is envelope-level referential integrity: a relationship must name
two known subjects, a registration for a physical entity must name its location,
a correction must name a committed event, and every event must belong to the
current epoch and facility generation.

## 2. Architecture

```
                     SubmittedObservation          (untrusted caller input)
                              |
                     validation + admission        (value types, policy)
                              |
   +--------------------------+---------------------------+
   |                          |                           |
   v                          v                           v
 segment frames          commit manifest            derived index
 (append-only log)     (authoritative watermark)   (rebuildable postings)
   |                          |                           |
   +----------> EventEnvelope <----------+                |
                    |                     |                |
                    v                     v                v
              replay / query        verify / recover   bounded queries
```

**Authoritative state** is the manifest plus the committed prefix of the segment
log. Everything else the process holds — subject registry, source watermarks,
duplicate identities, query postings — is derived from that log and is rebuilt
from it after any restart. The index is a locator, never a source of truth: a
record it names is always re-read from the log and re-validated before it reaches
a caller, so an index can never change an answer.

### Persistence model

A commit is durable when, and only when, a replacement manifest naming it has
been flushed into place. Frames always reach stable storage *before* the manifest
that acknowledges them, so a crash can lose an unacknowledged commit but can
never expose a commit whose bytes are missing. Recovery enforces both directions
of that rule; see [docs/FORMAT.md](docs/FORMAT.md).

## 3. Identity, generation and authority

All identities and counters are distinct types with no implicit conversions
between them. `FacilityGeneration`, `FacilityEpoch`, `SourceGeneration`,
`LedgerSequence` and `SegmentIndex` cannot be passed for one another, and none of
them has a default-constructed "zero" value: absence is `std::optional`.

| Concept | Meaning |
| --- | --- |
| `EventId` | caller-chosen identity of a submission; reuse with different content is a conflict |
| `IdempotencyToken` | optional retry token, independent of `EventId` |
| `LedgerId` | identity of one ledger directory, assigned once at creation |
| `LedgerSequence` | position in the authoritative order; the only ordering this library guarantees |
| `LogicalTick` | ledger-assigned ordinal, increasing by one per accepted event |
| `FacilityGeneration` | generation of the authoritative facility model |
| `FacilityEpoch` | window within which mutations are admitted |
| `SourceGeneration` | incarnation of a producing component; fences a restarted producer |
| `SubjectRef` | typed reference to a facility-model entity or an opaque ASI/DFI object |
| `AcceptedAt` | logical tick plus a monotonic reading; **never** a wall clock, never used for ordering |
| `Digest` | SHA-256 over canonical bytes; the per-event integrity checksum |

### Authority model

* **Only the ledger assigns** a sequence, a logical tick, an acceptance time and
  an integrity digest. A submission cannot name any of them, so sequence forgery
  is impossible by construction; a decoded record whose declared sequence
  disagrees with its position is reported as forgery and never returned.
* **At most one writable handle per directory**, enforced by an exclusive
  operating-system file lock held for the handle's lifetime. A second writer in
  the same or another process fails with `locked/ledger-locked`. A writable open
  claims a fresh `WriterIncarnation`, and commits re-check that the published
  manifest is still the one this handle last published.
* **Ordering is commit order.** Nothing in this library implies a wall-clock
  total order. A producer may assert its own wall clock and its own ordering, and
  the ledger records the claim as untrusted provenance.
* **Admission rejects staleness deterministically**, with a stable machine
  readable code for each reason:

| Situation | Error |
| --- | --- |
| facility generation behind the authoritative one | `stale-generation/stale-facility-generation` |
| facility generation ahead of the authoritative one | `conflict/future-facility-generation` |
| epoch not the open epoch | `stale-epoch/stale-epoch`, `stale-epoch/epoch-closed`, `stale-epoch/unknown-epoch` |
| producer incarnation superseded | `stale-source/stale-source-generation` |
| producer sequence did not advance | `stale-source/source-sequence-regression` |
| event identity reused with different content | `conflict/duplicate-event-id` |
| retry token reused with different content | `conflict/duplicate-idempotency-token` |
| subject lifecycle violated | `conflict/subject-already-registered`, `conflict/subject-retired`, `not-found/subject-not-registered` |
| dangling or self reference | `invalid-argument/dangling-subject-reference`, `invalid-argument/self-reference` |
| correction target absent | `not-found/correction-target-not-found` |

## 4. What the state machine enforces

* A ledger's first committed record is its own creation (`ledger-opened`), in
  facility generation 1, before any epoch exists.
* No facility event is admitted while no epoch is open. An epoch opens only as
  the successor of the latest one, and only while no other epoch is open.
* The facility generation advances only while no epoch is open, and only by
  exactly one step from the authoritative value.
* Subjects are registered before they are observed, mutated or retired; a retired
  subject accepts no further observation, mutation or relationship. Only
  registration and retirement change a subject's lifecycle — an observation about
  a subject leaves its state alone.
* Physical subject kinds (`rack`, `asset`, `facility-node`) must name a location
  subject that is already known.
* A relationship must name two known, distinct subjects and must not be asserted
  twice or retracted when it is not asserted.
* A correction must name a committed event and never modifies it: the corrected
  record keeps its bytes, identity, sequence and integrity digest.
* Every externally influenced size is bounded before allocation: payloads,
  batches, segments, query limits, scan windows, index postings, dedupe
  identities, subjects, sources, checkpoints and export sizes.

## 5. Durability, recovery and concurrency

**Durability.** A commit returns only after the segment flush and the atomic
publication of the replacement manifest. `LedgerOptions::durable_commits = false`
selects a mode that still writes and orders identically but issues no flush; it
is reported through `LedgerState::durable` and `LedgerStats`, and it is not
crash-durable. Commit returns `AppendOutcome`/`BatchOutcome` values whose
`duplicate` flag reports an idempotent retry.

**Recovery.** Opening walks the committed prefix, validates every frame checksum,
the record indexes, the ledger sequences and the SHA-256 chain from the segment
seed, and requires the walk to end exactly at the committed offset with the chain
the manifest records. Bytes past the watermark are an unacknowledged tail:
discarded under the default policy, refused under `kRefuseOnUncommittedTail`,
reported exactly either way. A log *shorter* than the watermark, or corrupt
inside the committed prefix, refuses the open — the ledger never presents damaged
or incomplete history as authoritative. `OpenMode::kDiagnose` opens such a
directory for inspection so that `verify()` can report the damage precisely.

**Concurrency.** Within a process any public method is safe to call from any
thread: mutations are serialised by one internal lock, and reads take no lock —
they observe the committed prefix, which only ever grows. A replay stream fixes
its upper bound when it is created, so a concurrent writer cannot extend it.
Across processes exactly one writable handle may exist; read-only handles take no
lock by default and are safe beside a live writer for the same reason, with
`ReaderLockPolicy::kShared` available to exclude writers instead.

Callbacks are never invoked while an internal lock that a callback could
re-enter is held. The one documented exception is `fsl::IFaultInjector`, a
test-support surface that runs inside the commit critical section and whose
contract is to terminate the process or set a flag, never to call back into the
ledger.

## 6. Building, testing, installing

Requires CMake 3.21+ and a C++20 compiler.

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure

cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Options: `FSL_BUILD_TESTS`, `FSL_BUILD_TOOLS`, `FSL_BUILD_EXAMPLES`,
`FSL_BUILD_BENCHMARKS`, `FSL_WARNINGS_AS_ERRORS` (default `ON`),
`FSL_ENABLE_ASAN`. First-party code compiles with `/W4 /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wold-style-cast -Werror`
elsewhere, with no suppression pragmas.

### Downstream consumption

```cmake
find_package(FacilityStateLedger 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE FacilityStateLedger::facility_state_ledger)
```

```sh
cmake --install build/release --prefix /some/prefix
```

`tests/downstream/` is an independent consumer that configures, builds and runs
against a freshly installed prefix from outside the source tree. It runs as part
of `ctest`, so `ctest` is also the distributability proof.

### Minimal use

```cpp
#include <fsl/ledger.hpp>
#include <fsl/payload.hpp>

auto created = fsl::Ledger::create("/var/lib/dccp/facility-ledger");
if (!created) { /* created.status() is machine readable */ }
fsl::Ledger ledger = std::move(created).value();

fsl::SubmittedObservationFields fields;
fields.kind = fsl::EventKind::kEpochOpened;
fields.event_id = fsl::EventId::from_words(0x1111, 0x2222);
fields.facility_generation = fsl::FacilityGeneration(1);
fields.epoch = fsl::FacilityEpoch(1);
fields.subject = fsl::SubjectRef::create(fsl::SubjectKind::kLedger, "facility-state-ledger").value();
fields.payload_schema = fsl::payload::required_schema(*fields.kind);
fields.payload_schema_version = fsl::payload::required_schema_version(*fields.kind);
fields.payload = fsl::payload::encode(
    fsl::payload::EpochOpened{fsl::FacilityEpoch(1), fsl::FacilityGeneration(1)});
fields.provenance = fsl::ProvenanceRecord::create(
    fsl::SourceComponentId::parse("dccp.facility-topology").value(),
    fsl::SourceGeneration::first(), std::nullopt, std::nullopt, {}).value();

auto observation = fsl::SubmittedObservation::create(std::move(fields));
if (observation) {
  auto outcome = ledger.append(observation.value());
  if (outcome) {
    // outcome->event.sequence(), .integrity(), .duplicate
  }
}
```

Complete worked examples are in `examples/`:
`facility_journal` (full lifecycle against the public API),
`audit_export` (canonical export and verification of an existing ledger), and
`crash_writer` (a writer that terminates at a named durability boundary, used by
the durability proofs).

## 7. Inspection CLI

```
fsl <command> --dir <path> [options]
```

| Command | Mutates | Purpose |
| --- | --- | --- |
| `inspect` | no | identity, watermark, generation, open epoch, segment and index counters |
| `segments`, `checkpoints` | no | structural metadata |
| `read --sequence N \| --event-id HEX` | no | one canonical audit record |
| `find --event-id HEX \| --token HEX` | no | locate a committed event by identity |
| `query ... --limit N` | no | bounded historical query with the observed query source |
| `replay [--from N] [--to N] [--kind K] [--limit N]` | no | deterministic replay as audit records |
| `verify [--scope ...] [--checkpoint N] [--no-index]` | no | integrity report and every finding |
| `export [--from N] [--to N] [--subject T/K]` | no | canonical audit export to stdout |
| `subjects`, `sources` | no | lifecycle and watermark views |
| `init` | yes | create a ledger |
| `append ...` | yes | commit one event through the ordinary admission path |
| `checkpoint`, `rotate` | yes | structural operations |
| `recover [--dry-run] [--rebuild-index]` | yes | explicit recovery, reportable without modifying anything |

Read-only commands open with `OpenMode::kReadOnly` and go through the same public
API a consumer uses. **No CLI command bypasses an authority, generation or
integrity check.** Exit codes are stable: `0` success, `1` usage error, `2`
operation failure (the `Status` is printed as `<category>/<code>: <message>`).

## 8. Validation performed

Everything below was run on this machine (Windows 11, MSVC 19.44, CMake 4.3,
Ninja 1.13) against the committed sources. Evidence classes are labelled: **REAL**
means the real library over real files and real operating-system processes,
**SYNTHETIC** means generated workloads, **UNSUPPORTED** means not exercised here.

| Area | Evidence |
| --- | --- |
| Unit and integration | 21 CTest executables, all passing in Release and Debug; ~15 000 assertions |
| Format round-trips | every frame, header, manifest, checkpoint and index structure round-trips; every single-byte alteration of a frame and a segment header is rejected |
| Admission state machine | epoch, generation, subject lifecycle, relationship, correction, source-incarnation and capacity paths, each asserted against its exact error code |
| Idempotency | duplicate by `EventId` and by token, conflict on content change, survival across restart and across an index rebuild, duplicate recognition after the epoch has moved on |
| Atomic batches | all-or-nothing with the committed prefix, the chain and the segment size unchanged after a rejected batch; ordering preserved; batch-internal duplicates refused |
| Ordering | committed sequences proven unique and contiguous; replay proven ascending with no gaps; a stream's bound proven fixed against a concurrent writer |
| Durability and recovery (**REAL**) | a reader *process* sees only the committed prefix; a second *process* is refused the writer lock with `locked/ledger-locked`; writer *processes* are killed at `after-encode`, `after-segment-write`, `after-segment-flush`, `before-manifest-publish` and `after-commit-ack` and the ledger is reopened from a fresh process each time |
| Corruption (**REAL**) | bit flips at sampled offsets across the committed region, tail forgery, foreign segments, wrong segment index, missing segment, absurd frame length, damaged manifests, damaged checkpoints, damaged index — each rejected or repaired deterministically |
| Concurrency | 8 threads × 25 appends all committed exactly once; 8 threads racing on one identity produce exactly one commit; readers stay consistent during writes; close during in-flight work rejects every later operation with `closed/ledger-closed` |
| Property tests | seeded xorshift64 workloads (seeds 1, 7, 42, 1234, 99991, 20260101, 3, 11, 31337) checking watermark, generation, epoch, replay equivalence, verification and reopen-state equality after every accepted operation |
| Malformed input | invalid UTF-8, control characters, path traversal in subject keys, unknown enum values, unsupported schema and format versions, oversized payloads, truncated and trailing bytes |
| Warnings | zero first-party warnings under `/W4 /WX` in Release and Debug |
| Sanitizers | see the limitation below |

### Genuine limitations

* **Platform validation.** All testing was performed on Windows with MSVC. The
  POSIX code path (positional I/O, `fsync`, directory flush, `flock`, `/dev/urandom`)
  is implemented and reviewed but was **not executed** in this environment: no
  POSIX toolchain was available. POSIX support is therefore **UNSUPPORTED** by
  evidence, not by design. Under `flock`, advisory locking over NFS is unreliable.
* **AddressSanitizer** was not run: the MSVC ASan runtime was not available for
  the exercised toolchain. No sanitizer result is claimed.
* **Single-writer model.** Exactly one writable handle per directory. Concurrent
  writers are not supported and are actively fenced; there is no multi-writer
  merge path.
* **The derived index is resident.** Postings and duplicate identities are held in
  memory, bounded by `max_index_postings` and `max_dedupe_entries`. When the
  posting budget is exhausted, affected queries fall back to a bounded linear
  scan (reported through `QueryPage::source`). When the dedupe budget is
  exhausted, appends are **rejected** rather than silently forgetting an
  identity. When `max_cached_frame_offsets` is exceeded, the oldest segments lose
  their offset tables and random access into them costs one bounded scan.
* **No compaction.** Segments are never rewritten or deleted, because discarding
  records would break the audit semantics this repository exists to provide.
  Checkpointing is implemented and never discards records; checkpoints beyond
  `max_checkpoints` are pruned, which discards anchors but no provenance.
* **Wall-clock time is not ordering.** Only the commit sequence orders events.
  Segment and checkpoint timestamps are informational.
* **Benchmark numbers** in the table below are **SYNTHETIC**: they characterise a
  generated workload on this machine, not production hardware.

## 9. Benchmarks

`fsl_bench` measures completed operations, never submission or enqueue latency,
and includes the durability flushes in every durable figure. One representative
Release run on this machine, 2 000 events, batch 64, 32 subjects, 32-byte
payloads, `max_segment_bytes` 64 MiB — **SYNTHETIC**:

| Scenario | Result |
| --- | --- |
| durable single append | 132 events/s; mean 7 571 µs, p99 13 164 µs per completed append; 4 000 durability flushes for 2 000 appends |
| durable batch append (64) | 6 688 events/s, 107 batches/s, 62.5 events per batch |
| volatile append (not crash-durable) | 219 events/s — 1.7× the durable rate, with zero flushes issued |
| replay | 316 593 events/s, 14.4 MiB/s, mean 3.2 µs per step, no sequence gaps |
| integrity verification (full) | 104 591 records/s, 34.3 MiB/s, mean 9.6 µs per record, index consistent |
| bounded query | read by sequence 6.1 µs mean; find by event id 7.1 µs mean; query by subject 397 µs mean; all pages served from index postings |
| reopen | 31 ms to open 6 038 committed events, 0 events replayed (the derived index was already current) |

Run it with:

```sh
build/release/bin/fsl_bench --scenario all --events 2000
build/release/bin/fsl_bench --scenario query --events 500 --csv results.csv
```

Every section restates its build, platform, compiler and workload context, and
labels its numbers SYNTHETIC.

## 10. Repository layout

```
include/fsl/      public headers (the only installed surface)
src/              implementation and internal detail headers
tools/fsl/        the inspection CLI
examples/         worked examples, compiled and run as part of validation
bench/            the benchmark executable
tests/            the test suite and the downstream consumer
docs/FORMAT.md    the normative on-disk format
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
