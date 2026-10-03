# Contributing to Facility State Ledger

Facility State Ledger is a Data Center Control Plane (DCCP) Tranche 1 repository
maintained by Summon Software Labs. Contributions are welcome under the terms of
the Apache License 2.0.

## Licensing of contributions

By submitting a contribution you agree that it is licensed under the Apache
License 2.0, as described in section 5 of the [LICENSE](LICENSE). There is **no**
Contributor License Agreement to sign and no copyright assignment. You keep the
copyright to your contribution.

Do not add `Co-authored-by` trailers, generated-by notices, or attribution
lines that you cannot justify; commit authorship is recorded by Git itself.

## Before you open a pull request

1. Build both configurations with warnings-as-errors:

   ```
   cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/release
   cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/debug
   ```

2. Run the complete test suite in both configurations:

   ```
   ctest --test-dir build/release --output-on-failure
   ctest --test-dir build/debug --output-on-failure
   ```


3. Keep the public API strongly typed. New identities, generations, epochs,
   revisions, and external references must be distinct types; do not introduce
   implicit conversions between them, and do not use sentinel values where an
   optional or a sum type is clearer.

4. Any change to the persisted segment format requires a format version bump, a
   migration note in `docs/FORMAT.md`, and an explicit compatibility statement.

5. New behaviour needs evidence. Tests assert observable behaviour, not
   implementation details, and every new invariant needs a test that fails when
   the invariant is violated.

## Code quality expectations

* C++20, standard library only for the shipped library. New third-party
  dependencies must be justified in the pull request and are normally rejected
  when the standard library suffices.
* Zero first-party warnings under `/W4 /WX` (MSVC) or `-Wall -Wextra -Wpedantic
  -Werror` (GCC/Clang).
* No TODO placeholders, dead code, debug prints, machine-specific absolute
  paths, or generated junk in the tree.
* Deterministic behaviour must be reproducible: seeded property tests, stable
  iteration order, canonical serialization.

## Reporting defects

Open an issue with the exact command, the observed result, and the expected
result. Durability, integrity, and ordering defects are treated as release
blockers; include the ledger directory (or a minimised reproduction) when the
defect is in recovery.
