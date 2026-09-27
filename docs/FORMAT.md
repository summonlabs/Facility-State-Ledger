# Facility State Ledger — on-disk format

This document is the normative description of the byte formats this library
writes. It is versioned with the source: a change to any layout below requires a
format version bump and a compatibility statement in this file.

All integers are little-endian. All digests are SHA-256. All checksums are
CRC-32C (Castagnoli). No structure contains padding: every field is written in
the order listed and the sizes are exact.

Three independent format versions exist and are versioned separately:

| Version constant | Value | Governs |
| --- | --- | --- |
| `kSegmentFormatVersion` | 1 | segment header and event frames |
| `kManifestFormatVersion` | 1 | the commit manifest |
| `kCheckpointFormatVersion` | 1 | checkpoints |
| `kIndexFormatVersion` | 1 | derived index segments |

## Ledger directory

```
<root>/ledger.manifest                          authoritative commit watermark
<root>/ledger.manifest.bak                      previous publication, for fallback
<root>/ledger.manifest.tmp                      staging file for the next publication
<root>/ledger.lock                              writer lock
<root>/segments/segment-<16 decimal digits>.fsl
<root>/index/index-<16 digits>-<8 digits>.idx
<root>/checkpoints/checkpoint-<20 digits>.fsl
```

Every name is fixed-width and lower-case, so the lexicographic order of a
directory listing is the numeric order of the objects. No name is ever derived
from untrusted input: segment, index and checkpoint names come from counters the
ledger itself assigns.

## Chain and integrity model

Two independent integrity mechanisms protect committed data.

* **Per-frame checksum.** Every frame ends with a four-byte commit marker and
  carries a CRC-32C over everything after its own checksum field. A frame is
  *complete* only when its declared length is present, the marker is exact and
  the checksum matches.
* **Per-segment hash chain.** Every frame carries a 32-byte chain value computed
  as `SHA-256(previous_chain || frame_bytes)`, where `frame_bytes` has the
  checksum and chain fields zeroed. The chain of the first frame of a segment
  starts from the segment seed
  `SHA-256(ledger_id || segment_index || base_sequence || format_version)`, which
  binds a segment's chain to its identity and position: frames cannot be moved
  between ledgers or between segments without detection.

The chain is *per segment*. Ordering across segments is established by the
`ledger_sequence` field of each event frame and by the segment `base_sequence`
recorded in the header and named by the manifest, not by a cross-segment chain.

## Event frame

```
offset  size  field
     0     4  frame_size          total bytes of the frame, >= 72
     4     4  frame_crc32c        CRC-32C over bytes [8, frame_size)
     8     1  frame_kind          1 event, 2 segment seal, 3 index entry, 4 index seal
     9     1  frame_version       must be 1
    10     2  frame_flags         reserved, must be zero
    12     8  record_index        1-based index of this frame within its segment
    20     8  ledger_sequence     0 when the kind carries no sequence
    28     8  logical_tick        0 when the kind carries no tick
    36    32  chain               running chain after this frame
    68     N  body                frame_size - 72 bytes
   N-4     4  commit_marker       0x54494D43 ("CMIT")
```

`frame_size` is bounded by 256 MiB; larger values are rejected before any read.

## Event body

```
u32  envelope_version            1
u64  ledger_sequence
u64  logical_tick
u64  monotonic_nanoseconds
32B  content_digest              SHA-256 over the submission encoding below
u8   has_idempotency_token
[16B idempotency_token]
u32  submission_version          1
     -- submission encoding follows --
```

### Submission encoding

The submission encoding is also the value hashed to produce `content_digest`, so
it contains only caller-controlled fields. The retry token is deliberately
excluded: two submissions that differ only in their token are the same content.

```
16B  event_id
u64  facility_generation
u8   has_epoch
[u64 epoch]
u8   event_kind
     subject reference:  u8 kind, u32 length, bytes key
u8   has_correction_target
[16B correction_target]
     payload schema:     u32 length, bytes identifier
u32  payload_schema_version
     payload:            u32 length, bytes payload
     provenance:
       u32 length, bytes source identifier
       u64 source_generation
       u8  has_source_sequence   [u64 source_sequence]
       u8  has_wall_clock        [u64 source_wall_clock_unix_nanos]
       u32 attribute_count
       attributes: u32 length, bytes key, u32 length, bytes value
```

Decoding re-derives the content digest from the decoded fields and requires it to
equal the stored one. A record whose content and digest disagree is rejected: it
is not a record this library ever wrote.

`has_epoch` must be false for `ledger-opened` and `generation-advanced`, and true
for every other kind; any other combination is rejected.

## Segment header

```
offset  size  field
     0     8  magic "FSLSEG\0\0"
     8     4  format_version        1
    12     2  header_size           64
    14     2  flags                 reserved, must be zero
    16    16  ledger_id
    32     8  segment_index         1-based, never zero
    40     8  base_sequence         ledger sequence of the segment's first event
    48     8  created_unix_nanos    informational only, never used for ordering
    56     4  reserved              must be zero
    60     4  header_crc32c         CRC-32C over bytes [0, 60)
```

`base_sequence` is carried in the header, and included in the segment chain seed,
so a segment cannot be replayed at a different position undetected.

## Segment seal frame

Written when a segment is rotated. Its body is:

```
u64 record_count   frames written before the seal
u64 first_sequence 0 when the segment holds no events
u64 last_sequence  0 when the segment holds no events
u64 sealed_offset  byte offset of the seal frame itself
```

The seal frame continues the segment chain, so a truncated or reordered seal is
detected by the same mechanism as any other frame.

## Commit manifest

The manifest is the authority for what is committed. It is published by writing
`ledger.manifest.tmp`, flushing it, and atomically replacing `ledger.manifest`,
keeping the previous publication at `ledger.manifest.bak`.

```
offset  size  field
     0     8  magic "FSLMAN\0\0"
     8     4  format_version
    12     4  manifest_size        252
    16     8  manifest_generation  increases by one on every publication
    24     8  writer_incarnation   increases on every writable open
    32    16  ledger_id
    48     8  flags                bit 0: commits were published with flushes
    56     8  committed_sequence   0 when nothing is committed
    64     8  committed_logical_tick
    72     8  committed_segment_index
    80     8  committed_offset     bytes of that segment that are committed
    88     8  committed_frame_index
    96    32  committed_chain      chain after the last committed frame
   128     8  active_segment_index
   136     8  active_segment_offset
   144     8  segment_count
   152     8  total_event_count
   160     8  facility_generation
   168     8  open_epoch           0 encodes "no epoch is open"
   176     8  latest_epoch
   184     8  index_generation
   192     8  index_through_sequence
   200     8  checkpoint_count
   208     8  latest_checkpoint_sequence
   216     4  crc32c               CRC-32C over bytes [0, 216)
   220    32  digest               SHA-256 over bytes [0, 220)
```

`open_epoch`, `committed_sequence` and `latest_checkpoint_sequence` use zero to
encode absence, because a manifest is a wire structure with fixed fields; the
public API models the same absence with `std::optional`.

### Recovery rules

Opening a ledger applies these rules in order:

1. The primary manifest is used if it decodes. Otherwise the backup is used, and
   the open report records `manifest_recovered_from_backup`.
2. If neither decodes, the open fails with `recovery-required/manifest-rebuilt`.
   The ledger cannot distinguish committed from uncommitted frames without it, so
   it refuses rather than promote an unacknowledged frame.
3. Segments `1..segment_count` must exist and their headers must name the same
   ledger identity and their own segment index.
4. The committed segment is walked from its header. Every frame must decode, the
   record indexes must be contiguous, the ledger sequences must be contiguous
   from `base_sequence`, and the chain must be continuous from the segment seed.
   The walk must end exactly at `committed_offset`, with the chain recorded in
   the manifest, and with the sequence recorded in the manifest. Any deviation is
   an integrity failure and the open is refused.
5. If the active segment is longer than the committed extent, the extra bytes are
   an unacknowledged tail. Under the default policy they are discarded and the
   file is truncated; the open report records the exact byte and frame counts.
   Under `kRefuseOnUncommittedTail` the open fails instead.
6. If the active segment is *shorter* than the committed extent, a committed
   record is missing: the open fails with `integrity-failure/committed-record-lost`
   under every policy.

## Checkpoint

```
offset  size  field
     0     8  magic "FSLCKP\0\0"
     8     4  format_version
    12     4  checkpoint_size
    16    16  ledger_id
    32     8  sequence
    40     8  logical_tick
    48     8  segment_index
    56     8  offset
    64    32  chain_at_sequence
    96     8  manifest_generation
   104     8  facility_generation
   112     8  open_epoch
   120     8  latest_epoch
   128     8  event_count
   136     8  created_unix_nanos
   144     4  crc32c
   148    32  digest
```

A checkpoint is derived data and an audit anchor. It never authorises an event
the manifest does not already name: a checkpoint whose sequence exceeds the commit
watermark is rejected. Checkpoints are pruned to the configured maximum; pruning
discards anchors, never provenance.

## Index entry

The derived index is a set of `(kind, key, sequence)` postings. It is written to
`index-<generation>-<ordinal>.idx` files using the same frame format.

```
u8   index_kind
u8   reserved          must be zero
u16  reserved          must be zero
u64  sequence
u64  extra             kind dependent
u64  aux               kind dependent
u32  key_size
     key bytes
```

| kind | value | key | extra | aux |
| --- | --- | --- | --- | --- |
| event-id | 1 | event id, exactly 16 bytes | event kind | – |
| idempotency | 2 | token, exactly 16 bytes | event kind | – |
| subject | 3 | canonical subject reference | event kind | – |
| source | 4 | source identifier text | source generation | source sequence |
| epoch | 5 | epoch, exactly 8 bytes | event kind | – |
| generation | 6 | generation, exactly 8 bytes | event kind | – |
| event-kind | 7 | event kind, exactly 1 byte | – | – |
| relationship | 8 | relationship triple | event kind | – |

A key whose width does not match its kind is rejected as malformed.

### Index authority

The index is never authoritative:

* A posting only ever narrows a search. The record it names is re-read from the
  log and re-validated before it reaches a caller, so an index can never change
  an answer.
* Entries whose sequence exceeds the commit watermark are discarded and the index
  file is truncated at that point.
* An index that cannot be read, or that disagrees with the log, is rebuilt from
  the authoritative log under a new generation. The old generation is removed
  only after the new one has been published.
* During a rebuild, derived lifecycle state is folded in commit order, not in
  posting order, so a superseded state can never be settled on.

## Relationship to the public API

Nothing above is part of the public API. Consumers construct
`fsl::SubmittedObservation` values, receive `fsl::EventEnvelope` values, and read
`fsl::SegmentInfo`, `fsl::CheckpointInfo` and canonical audit records. The raw
structures exist so that an operator can inspect or archive a ledger without this
library, and so that a format change is a reviewable event.
