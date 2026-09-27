// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <limits>

#include "detail/codec.hpp"
#include "detail/ledger_impl.hpp"
#include "fsl/payload.hpp"
#include "fsl/text.hpp"

/// \file ledger_open.cpp
/// Opening a ledger directory: manifest selection, committed-prefix recovery and
/// new-ledger initialisation.
///
/// The ordering rule that makes recovery sound is: the manifest names the
/// committed prefix, the log may hold bytes beyond it, and those bytes were
/// never acknowledged to any caller. Recovery therefore discards them rather
/// than adopting them, and refuses to open when the log is *shorter* than the
/// manifest says, because that means a committed record is gone.

namespace fsl::detail {
namespace {

constexpr std::size_t kMaxDirectoryEntries = 65536;
constexpr std::size_t kMaxManifestBytes = 4096;
constexpr std::size_t kReadChunk = 256u * 1024u;

[[nodiscard]] ErrorCode code_for_decode(FrameDecodeOutcome outcome) {
  switch (outcome) {
    case FrameDecodeOutcome::kOk:
      return ErrorCode::kOk;
    case FrameDecodeOutcome::kIncomplete:
      return ErrorCode::kTruncatedInput;
    case FrameDecodeOutcome::kCorrupt:
      return ErrorCode::kMidFileCorruption;
    case FrameDecodeOutcome::kUnsupportedVersion:
      return ErrorCode::kUnsupportedFormatVersion;
  }
  return ErrorCode::kInternalError;
}

[[nodiscard]] std::string segment_label(std::uint64_t index) {
  return "segment " + std::to_string(index);
}

}  // namespace

Status LedgerImpl::validate_options() const {
  if (options_.max_payload_bytes == 0 || options_.max_batch_events == 0 || options_.max_segment_bytes == 0 ||
      options_.max_query_limit == 0 || options_.max_scan_window == 0 || options_.max_dedupe_entries == 0 ||
      options_.max_index_postings == 0 || options_.max_subjects == 0 || options_.max_sources == 0 ||
      options_.max_export_events == 0 || options_.max_export_bytes == 0 ||
      options_.max_cached_frame_offsets == 0) {
    return invalid_argument(ErrorCode::kLimitOutOfRange, "every configured bound must be greater than zero");
  }
  if (options_.max_payload_bytes > ByteReader::kMaxSizedField) {
    return invalid_argument(ErrorCode::kLimitOutOfRange,
                            "max_payload_bytes exceeds the structural payload ceiling");
  }
  if (options_.max_segment_bytes < kSegmentHeaderSize + kFrameOverhead * 2) {
    return invalid_argument(ErrorCode::kLimitOutOfRange,
                            "max_segment_bytes is too small to hold even one frame");
  }
  return Status::ok();
}

void LedgerImpl::note(ErrorCode code, std::string message) {
  RecoveryNote entry;
  entry.code = code;
  entry.message = std::move(message);
  open_report_.notes.push_back(std::move(entry));
}

Result<std::shared_ptr<LedgerImpl>> LedgerImpl::open(const std::filesystem::path& directory,
                                                     OpenMode mode,
                                                     LedgerOptions options) {
  auto resolved = absolute_normalized(directory);
  if (!resolved.has_value()) {
    return resolved.status();
  }
  auto impl =
      std::shared_ptr<LedgerImpl>(new LedgerImpl(std::move(resolved).value(), mode, std::move(options)));
  Status status = impl->initialize();
  if (status.is_error()) {
    return status;
  }
  return impl;
}

LedgerImpl::LedgerImpl(std::filesystem::path root, OpenMode mode, LedgerOptions options)
    : root_(std::move(root)), paths_(root_), mode_(mode), options_(std::move(options)), index_(options_) {
  writable_ = mode_ == OpenMode::kReadWrite || mode_ == OpenMode::kCreate;
  durable_ = options_.durable_commits && writable_;
  clock_ = options_.clock != nullptr ? options_.clock : &default_clock_;
}

LedgerImpl::~LedgerImpl() { closed_.store(true, std::memory_order_release); }

Status LedgerImpl::initialize() {
  if (Status status = validate_options(); status.is_error()) {
    return status;
  }

  auto exists = path_exists(root_);
  if (!exists.has_value()) {
    return exists.status();
  }

  if (!exists.value()) {
    if (mode_ != OpenMode::kCreate) {
      return not_found(ErrorCode::kLedgerNotFound,
                       "no ledger directory at " + root_.string() +
                           "; open with OpenMode::kCreate to create one");
    }
    if (Status status = ensure_directory(root_); status.is_error()) {
      return status;
    }
  } else {
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error)) {
      return invalid_argument(ErrorCode::kNotARegularFile,
                              root_.string() + " exists and is not a directory");
    }
  }

  auto manifest_present = path_exists(paths_.manifest());
  if (!manifest_present.has_value()) {
    return manifest_present.status();
  }
  const bool needs_layout = mode_ == OpenMode::kCreate && !manifest_present.value();
  if (needs_layout) {
    for (const std::filesystem::path& directory :
         {paths_.segments_directory(), paths_.index_directory(), paths_.checkpoints_directory()}) {
      if (Status status = ensure_directory(directory); status.is_error()) {
        return status;
      }
    }
  }

  if (writable_) {
    auto lock = FileLock::acquire(paths_.lock_file(), LockMode::kExclusive, false);
    if (!lock.has_value()) {
      return lock.status();
    }
    writer_lock_ = std::move(lock).value();
  } else if (options_.reader_lock == ReaderLockPolicy::kShared) {
    auto lock = FileLock::acquire(paths_.lock_file(), LockMode::kShared, false);
    if (!lock.has_value()) {
      return lock.status();
    }
    writer_lock_ = std::move(lock).value();
  }

  if (!manifest_present.value()) {
    bool has_segment_files = false;
    std::error_code error;
    if (std::filesystem::is_directory(paths_.segments_directory(), error) && !error) {
      auto names = list_directory(paths_.segments_directory(), kMaxDirectoryEntries);
      if (names.has_value()) {
        for (const std::string& name : names.value()) {
          SegmentIndex parsed{1};
          if (Paths::parse_segment_name(name, parsed)) {
            has_segment_files = true;
            break;
          }
        }
      }
    }
    if (has_segment_files) {
      return recovery_required(
          ErrorCode::kManifestRebuilt,
          "the commit manifest for " + root_.string() +
              " is missing while segment files are present; committed and uncommitted frames cannot be "
              "told apart without it");
    }
    if (mode_ != OpenMode::kCreate) {
      return not_found(ErrorCode::kLedgerNotFound,
                       "no ledger at " + root_.string() + "; open with OpenMode::kCreate to initialise one");
    }
    return initialize_new_ledger();
  }

  if (Status status = load_manifest(); status.is_error()) {
    return status;
  }
  if (Status status = load_segments(); status.is_error()) {
    return status;
  }
  if (Status status = validate_committed_prefix(mode_ != OpenMode::kDiagnose, writable_); status.is_error()) {
    return status;
  }

  if (writable_) {
    auto next = manifest_.writer_incarnation;
    if (next == std::numeric_limits<std::uint64_t>::max()) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "the writer incarnation counter is exhausted");
    }
    manifest_.writer_incarnation = next + 1;
    if (Status status = publish_manifest(true); status.is_error()) {
      return status;
    }
  }
  writer_incarnation_ = WriterIncarnation(manifest_.writer_incarnation);

  if (mode_ == OpenMode::kDiagnose) {
    note(ErrorCode::kRecoveryPolicyRejected,
         "opened for diagnosis: the log was not walked and derived state may be incomplete");
    closed_.store(false, std::memory_order_release);
    return Status::ok();
  }

  if (Status status = load_index(); status.is_error()) {
    return status;
  }

  closed_.store(false, std::memory_order_release);
  return Status::ok();
}

Status LedgerImpl::initialize_new_ledger() {
  for (const std::filesystem::path& directory :
       {paths_.segments_directory(), paths_.index_directory(), paths_.checkpoints_directory()}) {
    if (Status status = ensure_directory(directory); status.is_error()) {
      return status;
    }
  }

  auto identity = generate_ledger_id();
  if (!identity.has_value()) {
    return identity.status();
  }

  manifest_ = Manifest{};
  manifest_.ledger_id = identity.value();
  manifest_.manifest_generation = 1;
  manifest_.writer_incarnation = 1;
  manifest_.flags = durable_ ? kManifestFlagDurable : 0;
  manifest_.committed_sequence = 0;
  manifest_.committed_logical_tick = 0;
  manifest_.committed_segment_index = 0;
  manifest_.committed_offset = 0;
  manifest_.committed_frame_index = 0;
  manifest_.committed_chain = Digest{};
  manifest_.active_segment_index = 1;
  manifest_.active_segment_offset = kSegmentHeaderSize;
  manifest_.segment_count = 1;
  manifest_.total_event_count = 0;
  manifest_.facility_generation = 1;
  manifest_.open_epoch = 0;
  manifest_.latest_epoch = 0;
  manifest_.index_generation = 1;
  manifest_.index_through_sequence = 0;
  manifest_.checkpoint_count = 0;
  manifest_.latest_checkpoint_sequence = 0;

  SegmentHeader header;
  header.format_version = kSegmentFormatVersion;
  header.ledger_id = manifest_.ledger_id;
  header.segment_index = 1;
  header.base_sequence = 1;
  header.created_unix_nanos = options_.segment_timestamp_unix_nanos.has_value()
                                  ? *options_.segment_timestamp_unix_nanos
                                  : clock_->unix_nanoseconds();

  {
    const std::filesystem::path path = paths_.segment(SegmentIndex(1));
    auto created = File::open_read_write_create(path);
    if (!created.has_value()) {
      return created.status();
    }
    File handle = std::move(created).value();
    const std::vector<std::uint8_t> bytes = encode_segment_header(header);
    if (Status status = handle.append(bytes); status.is_error()) {
      return status;
    }
    if (durable_) {
      if (Status status = handle.flush(); status.is_error()) {
        return status;
      }
      ++stats_.flush_calls;
    }
  }

  manifest_generation_ = ManifestGeneration(manifest_.manifest_generation);
  if (Status status = publish_manifest(true); status.is_error()) {
    return status;
  }

  if (Status status = load_segments(); status.is_error()) {
    return status;
  }
  // The derived index exists from the first commit, so a later reopen finds it
  // already current instead of replaying the whole log to rebuild it.
  if (Status status = start_index_generation(manifest_.index_generation); status.is_error()) {
    return status;
  }
  active_header_ = header;
  active_offset_ = kSegmentHeaderSize;
  active_record_index_ = 0;
  active_first_sequence_ = 1;
  active_chain_ = segment_chain_seed(header);
  open_report_.created = true;
  note(ErrorCode::kLedgerNotFound, "initialised a new ledger with identity " + manifest_.ledger_id.to_hex());

  // A newly created ledger is not yet usable for facility events: no epoch is
  // open. Its first committed record therefore documents its own creation.
  SubmittedObservationFields fields;
  fields.event_id = derive_creation_event_id(manifest_.ledger_id);
  fields.facility_generation = FacilityGeneration(1);
  fields.kind = EventKind::kLedgerOpened;
  fields.subject = SubjectRef::create(SubjectKind::kLedger, "facility-state-ledger").value();
  fields.payload_schema = payload::ledger_opened_schema();
  fields.payload_schema_version = payload::required_schema_version(EventKind::kLedgerOpened);
  payload::LedgerOpened opened;
  opened.ledger_id = manifest_.ledger_id;
  opened.segment_format_version = kSegmentFormatVersion;
  opened.manifest_generation = manifest_.manifest_generation;
  fields.payload = payload::encode(opened);
  auto source = SourceComponentId::parse("facility-state-ledger");
  if (!source.has_value()) {
    return source.status();
  }
  auto provenance = ProvenanceRecord::create(std::move(source).value(), SourceGeneration::first(),
                                             std::nullopt, std::nullopt, {});
  if (!provenance.has_value()) {
    return provenance.status();
  }
  fields.provenance = std::move(provenance).value();

  auto observation = SubmittedObservation::create(std::move(fields));
  if (!observation.has_value()) {
    return observation.status();
  }
  auto appended = append(observation.value());
  if (!appended.has_value()) {
    return appended.status();
  }
  return Status::ok();
}


Status LedgerImpl::read_manifest_file(const std::filesystem::path& path, Manifest& out) const {
  auto present = path_exists(path);
  if (!present.has_value()) {
    return present.status();
  }
  if (!present.value()) {
    return not_found(ErrorCode::kLedgerNotFound, "no manifest at " + path.string());
  }
  auto regular = fsl::detail::is_regular_file(path);
  if (!regular.has_value()) {
    return regular.status();
  }
  if (!regular.value()) {
    return invalid_argument(ErrorCode::kNotARegularFile, path.string() + " is not a regular file");
  }
  auto file = File::open_read(path);
  if (!file.has_value()) {
    return file.status();
  }
  File handle = std::move(file).value();
  auto size = handle.size();
  if (!size.has_value()) {
    return size.status();
  }
  if (size.value() > kMaxManifestBytes) {
    return malformed_input(ErrorCode::kSizeOutOfRange,
                           "manifest at " + path.string() + " is larger than any valid manifest");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.value()));
  auto read = handle.read_at(0, bytes);
  if (!read.has_value()) {
    return read.status();
  }
  if (read.value() != bytes.size()) {
    return malformed_input(ErrorCode::kTruncatedInput, "manifest at " + path.string() + " is truncated");
  }
  auto decoded = decode_manifest(bytes);
  if (!decoded.has_value()) {
    return decoded.status();
  }
  out = std::move(decoded).value();
  return Status::ok();
}

Status LedgerImpl::load_manifest() {
  auto primary = read_manifest_file(paths_.manifest(), manifest_);
  if (primary.is_ok()) {
    manifest_generation_ = ManifestGeneration(manifest_.manifest_generation);
    return Status::ok();
  }

  auto backup = read_manifest_file(paths_.manifest_backup(), manifest_);
  if (backup.is_ok()) {
    manifest_generation_ = ManifestGeneration(manifest_.manifest_generation);
    open_report_.manifest_recovered_from_backup = true;
    manifest_authority_trusted_ = false;
    note(ErrorCode::kManifestRebuilt, "the primary manifest was unusable (" + primary.to_string() +
                                          "); the previous publication was used instead");
    return Status::ok();
  }

  return recovery_required(ErrorCode::kManifestRebuilt,
                           "no usable manifest at " + root_.string() + ": primary reports '" +
                               primary.to_string() + "' and backup reports '" + backup.to_string() +
                               "'; the commit watermark cannot be established without one");
}

Status LedgerImpl::load_segments() {
  if (manifest_.segment_count == 0) {
    return integrity_failure(ErrorCode::kManifestAheadOfLog, "the manifest names zero segments");
  }
  if (manifest_.active_segment_index == 0 || manifest_.active_segment_index > manifest_.segment_count ||
      (manifest_.committed_sequence != 0 &&
       (manifest_.committed_segment_index == 0 ||
        manifest_.committed_segment_index > manifest_.segment_count))) {
    return integrity_failure(ErrorCode::kManifestAheadOfLog,
                             "the manifest names a segment outside the range it declares");
  }

  auto names = list_directory(paths_.segments_directory(), kMaxDirectoryEntries);
  if (!names.has_value()) {
    return names.status();
  }
  std::vector<std::uint64_t> present;
  for (const std::string& name : names.value()) {
    SegmentIndex parsed{1};
    if (Paths::parse_segment_name(name, parsed)) {
      present.push_back(parsed.value());
    }
  }
  std::sort(present.begin(), present.end());

  for (std::uint64_t index = 1; index <= manifest_.segment_count; ++index) {
    if (!std::binary_search(present.begin(), present.end(), index)) {
      return integrity_failure(ErrorCode::kSegmentMissing,
                               "the manifest names " + std::to_string(manifest_.segment_count) +
                                   " segments but " + segment_label(index) + " is absent");
    }
  }
  for (const std::uint64_t index : present) {
    if (index > manifest_.segment_count) {
      note(ErrorCode::kSegmentOutOfOrder,
           segment_label(index) + " exists beyond the segment count in the manifest and is ignored");
    }
  }

  segments_.clear();
  resident_frame_offsets_ = 0;
  for (std::uint64_t index = 1; index <= manifest_.segment_count; ++index) {
    const std::filesystem::path path = paths_.segment(SegmentIndex(index));
    auto opened = File::open_read(path);
    if (!opened.has_value()) {
      return opened.status();
    }
    File handle = std::move(opened).value();
    std::vector<std::uint8_t> header_bytes(kSegmentHeaderSize);
    auto read = handle.read_at(0, header_bytes);
    if (!read.has_value()) {
      return read.status();
    }
    if (read.value() != kSegmentHeaderSize) {
      return integrity_failure(ErrorCode::kTruncatedInput,
                               segment_label(index) + " is shorter than a segment header");
    }
    auto header = decode_segment_header(header_bytes);
    if (!header.has_value()) {
      return header.status();
    }
    if (header->ledger_id != manifest_.ledger_id) {
      return integrity_failure(ErrorCode::kLedgerIdentityMismatch,
                               segment_label(index) + " belongs to a different ledger identity");
    }
    if (header->segment_index != index) {
      return integrity_failure(ErrorCode::kSegmentIdentityMismatch,
                               segment_label(index) + " declares segment index " +
                                   std::to_string(header->segment_index));
    }
    segments_.push_back(std::make_shared<SegmentDescriptor>(index, header->base_sequence,
                                                            kSegmentHeaderSize, std::move(handle)));
  }

  if (writable_) {
    const std::filesystem::path path = paths_.segment(SegmentIndex(manifest_.active_segment_index));
    auto opened = File::open_read_write(path);
    if (!opened.has_value()) {
      return opened.status();
    }
    active_file_ = std::move(opened).value();
    std::vector<std::uint8_t> header_bytes(kSegmentHeaderSize);
    auto read = active_file_.read_at(0, header_bytes);
    if (!read.has_value() || read.value() != kSegmentHeaderSize) {
      return integrity_failure(ErrorCode::kSegmentIdentityMismatch,
                               "the active segment header became unreadable during open");
    }
    auto header = decode_segment_header(header_bytes);
    if (!header.has_value()) {
      return header.status();
    }
    active_header_ = std::move(header).value();
  }
  return Status::ok();
}

LedgerImpl::WalkResult LedgerImpl::walk_segment(const SegmentDescriptor& descriptor,
                                               std::uint64_t start_offset,
                                               std::uint64_t expected_sequence,
                                               const Digest& start_chain,
                                               std::uint64_t stop_offset,
                                               std::size_t max_records,
                                               bool expect_commit_at_stop,
                                               std::vector<std::uint64_t>* offsets) const {
  WalkResult result;
  Digest chain = start_chain;
  result.chain = chain;
  result.end_offset = start_offset;

  auto size = const_cast<File&>(descriptor.file).size();
  if (!size.has_value()) {
    result.finding = VerifyFinding{size.status().code(), size.status().message(), std::nullopt,
                                   SegmentIndex(descriptor.index), start_offset};
    return result;
  }
  const std::uint64_t file_size = size.value();
  const std::uint64_t limit = stop_offset == 0 ? file_size : std::min(stop_offset, file_size);

  std::uint64_t offset = start_offset;
  std::uint64_t sequence = expected_sequence;
  std::uint64_t last_record_index = 0;
  std::vector<std::uint8_t> buffer;
  std::size_t buffer_base = 0;
  std::size_t buffer_length = 0;

  while (offset < limit) {
    if (max_records != 0 && result.records >= max_records) {
      break;
    }
    const bool buffered = offset >= buffer_base && (offset - buffer_base) + kFrameOverhead <= buffer_length;
    if (!buffered) {
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(limit - offset, kReadChunk));
      if (want < kFrameOverhead) {
        result.finding = VerifyFinding{expect_commit_at_stop ? ErrorCode::kCommittedRecordLost
                                                             : ErrorCode::kTruncatedInput,
                                       "fewer bytes remain than the smallest possible frame",
                                       std::nullopt, SegmentIndex(descriptor.index), offset};
        return result;
      }
      buffer.assign(want, 0);
      auto read = const_cast<File&>(descriptor.file).read_at(offset, buffer);
      if (!read.has_value()) {
        result.finding = VerifyFinding{read.status().code(), read.status().message(), std::nullopt,
                                       SegmentIndex(descriptor.index), offset};
        return result;
      }
      if (read.value() != want) {
        result.finding = VerifyFinding{ErrorCode::kCommittedRecordLost,
                                       "the segment ends inside the region being validated", std::nullopt,
                                       SegmentIndex(descriptor.index), offset};
        return result;
      }
      buffer_base = static_cast<std::size_t>(offset);
      buffer_length = read.value();
    }

    const std::size_t within = static_cast<std::size_t>(offset) - buffer_base;
    DecodedFrame frame;
    const std::span<const std::uint8_t> available(buffer.data() + within, buffer_length - within);
    const FrameDecodeOutcome outcome = decode_frame(available, frame);
    if (outcome == FrameDecodeOutcome::kIncomplete && buffer_base + buffer_length < limit) {
      // The frame is larger than the buffer; grow and retry.
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(
          limit - offset, std::max<std::uint64_t>(buffer_length * 2, kReadChunk)));
      buffer.assign(want, 0);
      auto read = const_cast<File&>(descriptor.file).read_at(offset, buffer);
      if (!read.has_value() || read.value() != want) {
        result.finding = VerifyFinding{ErrorCode::kFileReadFailed,
                                       "cannot read a full frame while validating the segment",
                                       std::nullopt, SegmentIndex(descriptor.index), offset};
        return result;
      }
      buffer_base = static_cast<std::size_t>(offset);
      buffer_length = read.value();
      continue;
    }
    if (outcome != FrameDecodeOutcome::kOk) {
      result.finding = VerifyFinding{code_for_decode(outcome),
                                     outcome == FrameDecodeOutcome::kIncomplete
                                         ? "the frame is incomplete"
                                         : (outcome == FrameDecodeOutcome::kUnsupportedVersion
                                                ? "the frame uses an unsupported format version"
                                                : "the frame is corrupt"),
                                     std::nullopt, SegmentIndex(descriptor.index), offset};
      return result;
    }
    if (last_record_index != 0 && frame.header.record_index != last_record_index + 1) {
      result.finding = VerifyFinding{ErrorCode::kSequenceNotMonotonic,
                                     "frame record index " + std::to_string(frame.header.record_index) +
                                         " does not follow " + std::to_string(last_record_index),
                                     std::nullopt, SegmentIndex(descriptor.index), offset};
      return result;
    }
    last_record_index = frame.header.record_index;

    if (frame.header.kind == FrameKind::kEvent) {
      if (sequence == 0 || frame.header.ledger_sequence != sequence) {
        result.finding = VerifyFinding{frame.header.ledger_sequence < sequence ? ErrorCode::kSequenceNotMonotonic
                                                                              : ErrorCode::kSequenceGap,
                                       "expected ledger sequence " + std::to_string(sequence) +
                                           " but the frame declares " +
                                           std::to_string(frame.header.ledger_sequence),
                                       LedgerSequence(frame.header.ledger_sequence),
                                       SegmentIndex(descriptor.index), offset};
        return result;
      }
      ++sequence;
      ++result.records;
      result.last_sequence = frame.header.ledger_sequence;
    } else if (frame.header.kind == FrameKind::kSegmentSeal) {
      auto seal = decode_segment_seal(frame.body);
      if (!seal.has_value()) {
        result.finding = VerifyFinding{seal.status().code(), seal.status().message(), std::nullopt,
                                       SegmentIndex(descriptor.index), offset};
        return result;
      }
      if (seal->sealed_offset != offset) {
        result.finding = VerifyFinding{ErrorCode::kHeaderFieldInvalid,
                                       "the seal frame names offset " +
                                           std::to_string(seal->sealed_offset) + " but sits at " +
                                           std::to_string(offset),
                                       std::nullopt, SegmentIndex(descriptor.index), offset};
        return result;
      }
    }

    const Digest recomputed =
        compute_frame_chain(chain, std::span<const std::uint8_t>(available.data(), frame.header.frame_size));
    if (recomputed != frame.header.chain) {
      result.finding =
          VerifyFinding{ErrorCode::kChainMismatch, "the record chain does not match at this offset",
                        std::nullopt, SegmentIndex(descriptor.index), offset};
      return result;
    }
    chain = frame.header.chain;
    if (offsets != nullptr) {
      offsets->push_back(offset);
    }
    result.bytes += frame.header.frame_size;
    offset += frame.header.frame_size;
    result.end_offset = offset;
  }

  result.chain = chain;
  if (expect_commit_at_stop && stop_offset != 0 && offset != stop_offset) {
    result.finding = VerifyFinding{ErrorCode::kCommittedRecordLost,
                                   "the committed prefix ends at offset " + std::to_string(stop_offset) +
                                       " but no frame boundary was found there",
                                   std::nullopt, SegmentIndex(descriptor.index), offset};
  }
  return result;
}

std::shared_ptr<SegmentDescriptor> LedgerImpl::segment_for(std::uint64_t sequence) const {
  std::shared_lock<std::shared_mutex> guard(segments_mutex_);
  for (const auto& descriptor : segments_) {
    const std::uint64_t base = descriptor->base_sequence;
    const std::uint64_t end = descriptor->end_sequence.load(std::memory_order_acquire);
    if (base == 0 || end == 0) {
      continue;
    }
    if (sequence >= base && sequence <= end) {
      return descriptor;
    }
  }
  return nullptr;
}

std::shared_ptr<const std::vector<std::shared_ptr<SegmentDescriptor>>> LedgerImpl::segment_snapshot() const {
  std::shared_lock<std::shared_mutex> guard(segments_mutex_);
  return std::make_shared<const std::vector<std::shared_ptr<SegmentDescriptor>>>(segments_);
}

Status LedgerImpl::validate_committed_prefix(bool full_scan, bool truncate_tail) {
  std::shared_ptr<SegmentDescriptor> active;
  std::shared_ptr<SegmentDescriptor> committed;
  for (const auto& descriptor : segments_) {
    if (descriptor->index == manifest_.active_segment_index) {
      active = descriptor;
    }
    if (manifest_.committed_sequence != 0 && descriptor->index == manifest_.committed_segment_index) {
      committed = descriptor;
    }
  }
  if (active == nullptr) {
    return integrity_failure(ErrorCode::kSegmentMissing,
                             "the active segment named by the manifest is not open");
  }

  for (const auto& descriptor : segments_) {
    descriptor->end_sequence.store(0, std::memory_order_relaxed);
    descriptor->end_offset.store(kSegmentHeaderSize, std::memory_order_relaxed);
  }

  // Segments that precede the committed one hold a fully committed prefix. Their
  // extent follows from the base sequence of the segment that follows them, so
  // random access into them after a reopen needs no extra walk.
  for (std::size_t i = 0; i + 1 < segments_.size(); ++i) {
    const auto& descriptor = segments_[i];
    const auto& next = segments_[i + 1];
    if (descriptor->base_sequence == 0 || next->base_sequence == 0) {
      continue;
    }
    if (manifest_.committed_sequence != 0 && descriptor->index >= manifest_.committed_segment_index) {
      continue;
    }
    auto size = descriptor->file.size();
    if (!size.has_value()) {
      return size.status();
    }
    descriptor->end_sequence.store(next->base_sequence - 1, std::memory_order_release);
    descriptor->end_offset.store(size.value(), std::memory_order_release);
    descriptor->sealed.store(true, std::memory_order_release);
  }

  if (manifest_.committed_sequence != 0) {
    if (committed == nullptr) {
      return integrity_failure(ErrorCode::kSegmentMissing,
                               "the manifest commits a sequence in " +
                                   segment_label(manifest_.committed_segment_index) +
                                   " which is not open");
    }
    auto size = committed->file.size();
    if (!size.has_value()) {
      return size.status();
    }
    if (size.value() < manifest_.committed_offset) {
      return integrity_failure(ErrorCode::kCommittedRecordLost,
                               segment_label(committed->index) + " holds " + std::to_string(size.value()) +
                                   " bytes but the manifest commits " +
                                   std::to_string(manifest_.committed_offset) +
                                   " bytes; a committed record is missing");
    }

    if (full_scan) {
      const Digest seed =
          segment_chain_seed(SegmentHeader{kSegmentFormatVersion, manifest_.ledger_id, committed->index,
                                           committed->base_sequence, 0});
      std::vector<std::uint64_t> offsets;
      offsets.reserve(1024);
      const WalkResult walked =
          walk_segment(*committed, kSegmentHeaderSize, committed->base_sequence, seed,
                       manifest_.committed_offset, 0, true, &offsets);
      if (walked.finding.has_value()) {
        return integrity_failure(walked.finding->code,
                                 "committed prefix validation failed in " +
                                     segment_label(committed->index) + " at offset " +
                                     std::to_string(walked.finding->offset) + ": " +
                                     walked.finding->message);
      }
      if (walked.last_sequence != manifest_.committed_sequence) {
        return integrity_failure(ErrorCode::kCommittedRecordLost,
                                 "the committed prefix ends at sequence " +
                                     std::to_string(walked.last_sequence) + " but the manifest commits " +
                                     std::to_string(manifest_.committed_sequence));
      }
      if (walked.chain != manifest_.committed_chain) {
        return integrity_failure(ErrorCode::kChainMismatch,
                                 "the committed chain does not agree with the manifest");
      }
      {
        std::unique_lock<std::shared_mutex> guard(segments_mutex_);
        committed->frame_offsets = std::move(offsets);
        resident_frame_offsets_ += committed->frame_offsets.size();
      }
    }
    committed->end_sequence.store(manifest_.committed_sequence, std::memory_order_release);
    committed->end_offset.store(manifest_.committed_offset, std::memory_order_release);
    committed->frame_count.store(committed->frame_offsets.size(), std::memory_order_release);
  }

  // This pass reports its own findings; an earlier pass must not leak into it.
  open_report_.truncated_tail_bytes = 0;
  open_report_.truncated_tail_frames = 0;
  auto active_size = active->file.size();
  if (!active_size.has_value()) {
    return active_size.status();
  }
  const std::uint64_t committed_end =
      (manifest_.committed_sequence != 0 && manifest_.committed_segment_index == manifest_.active_segment_index)
          ? manifest_.committed_offset
          : kSegmentHeaderSize;

  if (active_size.value() > committed_end) {
    const std::uint64_t tail_bytes = active_size.value() - committed_end;
    if (options_.recovery == RecoveryPolicy::kRefuseOnUncommittedTail && writable_) {
      return recovery_required(
          ErrorCode::kRecoveryPolicyRejected,
          segment_label(active->index) + " holds " + std::to_string(tail_bytes) +
              " bytes beyond the commit watermark and the recovery policy refuses to discard them");
    }
    // Count the frames in the tail so that the report is exact rather than
    // approximate. Findings are expected here: the tail is by definition not a
    // complete commit, so only the frame count is used.
    const Digest tail_seed = (manifest_.committed_sequence != 0 &&
                              manifest_.committed_segment_index == active->index)
                                 ? manifest_.committed_chain
                                 : segment_chain_seed(
                                       SegmentHeader{kSegmentFormatVersion, manifest_.ledger_id,
                                                     active->index, active->base_sequence, 0});
    const WalkResult tail_walk =
        walk_segment(*active, committed_end,
                     manifest_.committed_sequence != 0 &&
                             manifest_.committed_segment_index == active->index
                         ? manifest_.committed_sequence + 1
                         : active->base_sequence,
                     tail_seed, 0, 0, false, nullptr);
    open_report_.truncated_tail_bytes = tail_bytes;
    open_report_.truncated_tail_frames = tail_walk.records;
    open_report_.recovered = true;
    note(ErrorCode::kUncommittedTailDiscarded,
         std::to_string(tail_bytes) + " unacknowledged bytes beyond the commit watermark in " +
             segment_label(active->index) + " were discarded");
    if (truncate_tail) {
      auto writable_active = File::open_read_write(paths_.segment(SegmentIndex(active->index)));
      if (!writable_active.has_value()) {
        return writable_active.status();
      }
      File handle = std::move(writable_active).value();
      if (Status status = handle.truncate(committed_end); status.is_error()) {
        return status;
      }
      if (durable_) {
        if (Status status = handle.flush(); status.is_error()) {
          return status;
        }
        stats_.flush_calls += 1;
      }
    }
    active->end_sequence.store(manifest_.committed_sequence != 0 &&
                                       manifest_.committed_segment_index == active->index
                                   ? manifest_.committed_sequence
                                   : 0,
                               std::memory_order_release);
    active->end_offset.store(committed_end, std::memory_order_release);
  } else if (active_size.value() < committed_end) {
    return integrity_failure(ErrorCode::kCommittedRecordLost,
                             segment_label(active->index) + " holds " +
                                 std::to_string(active_size.value()) +
                                 " bytes but the commit watermark is at " +
                                 std::to_string(committed_end));
  }

  set_committed_state(manifest_.committed_sequence, manifest_.committed_logical_tick,
                      manifest_.committed_segment_index, manifest_.committed_offset,
                      manifest_.committed_frame_index, manifest_.committed_chain,
                      manifest_.total_event_count);

  if (writable_) {
    const std::uint64_t base = active->base_sequence;
    active_header_ = SegmentHeader{kSegmentFormatVersion, manifest_.ledger_id, active->index, base, 0};
    const bool active_holds_commit =
        manifest_.committed_sequence != 0 && manifest_.committed_segment_index == active->index;
    active_offset_ = active_holds_commit ? manifest_.committed_offset : kSegmentHeaderSize;
    active_record_index_ = active_holds_commit ? manifest_.committed_frame_index : 0;
    active_first_sequence_ = base;
    active_chain_ = active_holds_commit ? manifest_.committed_chain : segment_chain_seed(active_header_);
  }
  return Status::ok();
}

void LedgerImpl::set_committed_state(std::uint64_t sequence,
                                     std::uint64_t logical_tick,
                                     std::uint64_t segment_index,
                                     std::uint64_t offset,
                                     std::uint64_t frame_index,
                                     const Digest& chain,
                                     std::uint64_t event_count) {
  manifest_.committed_sequence = sequence;
  manifest_.committed_logical_tick = logical_tick;
  manifest_.committed_segment_index = segment_index;
  manifest_.committed_offset = offset;
  manifest_.committed_frame_index = frame_index;
  manifest_.committed_chain = chain;
  manifest_.total_event_count = event_count;
  manifest_.active_segment_offset = (segment_index == manifest_.active_segment_index) ? offset
                                                                                     : kSegmentHeaderSize;
  committed_sequence_.store(sequence, std::memory_order_release);
  committed_logical_tick_.store(logical_tick, std::memory_order_release);
  committed_segment_.store(segment_index, std::memory_order_release);
  committed_offset_.store(offset, std::memory_order_release);
  committed_frame_index_.store(frame_index, std::memory_order_release);
  event_count_.store(event_count, std::memory_order_release);
}

Status LedgerImpl::request_recovery(const std::string& message) const {
  return recovery_required(ErrorCode::kRecoveryPolicyRejected, message);
}

}  // namespace fsl::detail
