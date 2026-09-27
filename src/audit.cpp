// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>

#include "detail/codec.hpp"
#include "detail/ledger_impl.hpp"
#include "fsl/payload.hpp"
#include "fsl/text.hpp"

/// \file audit.cpp
/// Canonical audit export.
///
/// The export format is versioned, line-delimited JSON with a fixed key order
/// and no insignificant whitespace. Equal ledger content therefore produces
/// byte-identical output on every platform and every run, which is what makes an
/// exported file usable as an audit artefact: it can be diffed, hashed and
/// compared across sites. When more than one record is emitted the output is
/// prefixed by a header line carrying the ledger identity and the record count,
/// and suffixed by a trailer carrying the chain value at the last exported
/// sequence, so a consumer can verify that it received a contiguous prefix.

namespace fsl::detail {
namespace {

constexpr std::string_view kAuditFormat = "fsl.audit.v1";

void append_json_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20u) {
          constexpr char kHex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[(byte >> 4) & 0x0Fu]);
          out.push_back(kHex[byte & 0x0Fu]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_u64(std::string& out, std::uint64_t value) { out.append(std::to_string(value)); }

void append_optional_u64(std::string& out, const std::optional<std::uint64_t>& value) {
  if (value.has_value()) {
    append_u64(out, *value);
  } else {
    out.append("null");
  }
}

}  // namespace

std::string render_audit_record(const EventEnvelope& envelope) {
  std::string out;
  out.reserve(envelope.payload().size() * 2 + 768);
  out.push_back('{');
  out.append("\"format\":");
  append_json_string(out, kAuditFormat);
  out.append(",\"sequence\":");
  append_u64(out, envelope.sequence().value());
  out.append(",\"logical_tick\":");
  append_u64(out, envelope.accepted_at().logical_tick.value());
  out.append(",\"monotonic_nanoseconds\":");
  append_u64(out, envelope.accepted_at().monotonic_nanoseconds);
  out.append(",\"event_id\":");
  append_json_string(out, envelope.event_id().to_hex());
  out.append(",\"idempotency_token\":");
  if (envelope.idempotency_token().has_value()) {
    append_json_string(out, envelope.idempotency_token()->to_hex());
  } else {
    out.append("null");
  }
  out.append(",\"facility_generation\":");
  append_u64(out, envelope.facility_generation().value());
  out.append(",\"epoch\":");
  if (envelope.epoch().has_value()) {
    append_u64(out, envelope.epoch()->value());
  } else {
    out.append("null");
  }
  out.append(",\"kind\":");
  append_json_string(out, to_string(envelope.kind()));
  out.append(",\"subject\":");
  append_json_string(out, envelope.subject().to_string());
  out.append(",\"subject_kind\":");
  append_json_string(out, to_string(envelope.subject().kind()));
  out.append(",\"subject_key\":");
  append_json_string(out, envelope.subject().key().view());
  out.append(",\"correction_target\":");
  if (envelope.correction_target().has_value()) {
    append_json_string(out, envelope.correction_target()->to_hex());
  } else {
    out.append("null");
  }
  out.append(",\"payload_schema\":");
  append_json_string(out, envelope.payload_schema().view());
  out.append(",\"payload_schema_version\":");
  append_u64(out, envelope.payload_schema_version().value());
  out.append(",\"payload_hex\":");
  append_json_string(out, to_hex(envelope.payload()));
  out.append(",\"content_digest\":");
  append_json_string(out, envelope.content_digest().to_hex());
  out.append(",\"integrity\":");
  append_json_string(out, envelope.integrity().to_hex());
  out.append(",\"provenance\":{");
  out.append("\"source\":");
  append_json_string(out, envelope.provenance().source().view());
  out.append(",\"source_generation\":");
  append_u64(out, envelope.provenance().source_generation().value());
  out.append(",\"source_sequence\":");
  append_optional_u64(out, envelope.provenance().source_sequence().has_value()
                               ? std::optional<std::uint64_t>(envelope.provenance().source_sequence()->value())
                               : std::nullopt);
  out.append(",\"source_wall_clock_unix_nanos\":");
  append_optional_u64(out, envelope.provenance().source_wall_clock_unix_nanos());
  out.append(",\"attributes\":[");
  bool first = true;
  for (const ProvenanceAttribute& attribute : envelope.provenance().attributes()) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.push_back('{');
    out.append("\"key\":");
    append_json_string(out, attribute.key);
    out.append(",\"value\":");
    append_json_string(out, attribute.value);
    out.push_back('}');
  }
  out.append("]}");
  out.push_back('}');
  return out;
}

Result<void> LedgerImpl::export_audit(const AuditExportRequest& request, IAuditSink& sink) const {
  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  const std::size_t max_events =
      request.max_events.has_value() ? *request.max_events : options_.max_export_events;
  if (max_events == 0) {
    return invalid_argument(ErrorCode::kLimitRequired, "an audit export must allow at least one record");
  }
  if (upper == 0) {
    return not_found(ErrorCode::kNotFound, "the ledger holds no committed events to export");
  }
  const std::uint64_t low = request.from.has_value() ? request.from->value() : 1;
  const std::uint64_t high = request.to.has_value() ? std::min(request.to->value(), upper) : upper;
  if (low > high) {
    return invalid_argument(ErrorCode::kRangeInvalid,
                            "the export range starts after it ends: " + std::to_string(low) + " > " +
                                std::to_string(high));
  }

  // Collect the sequences to emit first so that the header can carry an exact
  // count and the trailer an exact chain value.
  std::vector<std::uint64_t> sequences;
  for (std::uint64_t sequence = low; sequence <= high; ++sequence) {
    auto envelope = read_committed(LedgerSequence(sequence));
    if (!envelope.has_value()) {
      return envelope.status();
    }
    if (request.subject.has_value() && !(envelope->subject() == *request.subject)) {
      continue;
    }
    sequences.push_back(sequence);
    if (sequences.size() >= max_events) {
      break;
    }
  }
  if (sequences.empty()) {
    return not_found(ErrorCode::kNotFound, "no committed record matches this export request");
  }

  std::size_t emitted = 0;
  std::string buffer;
  {
    buffer.append("{\"format\":");
    append_json_string(buffer, kAuditFormat);
    buffer.append(",\"record\":\"header\",\"ledger_id\":");
    append_json_string(buffer, manifest_.ledger_id.to_hex());
    buffer.append(",\"first_sequence\":");
    append_u64(buffer, low);
    buffer.append(",\"last_sequence\":");
    append_u64(buffer, high);
    buffer.append(",\"record_count\":");
    append_u64(buffer, sequences.size());
    buffer.append("}\n");
    if (buffer.size() > options_.max_export_bytes) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "the configured export byte ceiling is too small for an export header");
    }
    if (!sink.write(buffer)) {
      return io_failure(ErrorCode::kFileWriteFailed, "the audit sink rejected the export header");
    }
    emitted += buffer.size();
  }

  Digest chain;
  for (const std::uint64_t sequence : sequences) {
    auto envelope = read_committed(LedgerSequence(sequence));
    if (!envelope.has_value()) {
      return envelope.status();
    }
    buffer.assign(render_audit_record(envelope.value()));
    buffer.push_back('\n');
    if (emitted + buffer.size() > options_.max_export_bytes) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "the export reached the configured byte ceiling of " +
                                   std::to_string(options_.max_export_bytes) +
                                   " bytes after " + std::to_string(sequences.size()) + " records");
    }
    if (!sink.write(buffer)) {
      return io_failure(ErrorCode::kFileWriteFailed,
                        "the audit sink rejected a record at ledger sequence " +
                            std::to_string(sequence));
    }
    emitted += buffer.size();
    chain = envelope->integrity();
  }

  buffer.assign("{\"format\":");
  append_json_string(buffer, kAuditFormat);
  buffer.append(",\"record\":\"trailer\",\"exported\":");
  append_u64(buffer, sequences.size());
  buffer.append(",\"last_sequence\":");
  append_u64(buffer, sequences.back());
  buffer.append(",\"complete\":");
  buffer.append(sequences.size() == static_cast<std::size_t>(high - low + 1) ? "true" : "false");
  buffer.append(",\"last_record_integrity\":");
  append_json_string(buffer, chain.to_hex());
  buffer.append("}\n");
  if (emitted + buffer.size() > options_.max_export_bytes) {
    return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                             "the export reached the configured byte ceiling before its trailer");
  }
  if (!sink.write(buffer)) {
    return io_failure(ErrorCode::kFileWriteFailed, "the audit sink rejected the export trailer");
  }
  return Status::ok();
}

Result<std::string> LedgerImpl::export_audit_json(const AuditExportRequest& request) const {
  std::string output;
  struct StringSink final : IAuditSink {
    std::string* target = nullptr;
    bool write(std::string_view record) override {
      target->append(record);
      return true;
    }
  };
  StringSink sink;
  sink.target = &output;
  if (Status status = export_audit(request, sink); status.is_error()) {
    return status;
  }
  return output;
}

}  // namespace fsl::detail
