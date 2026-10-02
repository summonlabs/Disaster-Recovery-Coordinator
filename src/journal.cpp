#include "drc/journal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "codec.hpp"
#include "drc/canonical.hpp"
#include "drc/fileio.hpp"

namespace drc::journal {
namespace {

constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetVersion = 4;
constexpr std::size_t kOffsetFlags = 6;
constexpr std::size_t kOffsetHeaderSize = 8;
constexpr std::size_t kOffsetPayloadLength = 12;
constexpr std::size_t kOffsetSequence = 16;
constexpr std::size_t kOffsetType = 24;
constexpr std::size_t kOffsetBodyCrc = 28;
constexpr std::size_t kOffsetHeaderCrc = 32;
constexpr std::size_t kOffsetReserved = 36;
constexpr std::size_t kOffsetReserved2 = 40;
constexpr std::size_t kOffsetChain = 48;
constexpr std::size_t kMaxPayloadBytes = canonical::kMaxRecordBytes;

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu);
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                      (static_cast<std::uint16_t>(in[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (8u * i);
    }
    return value;
}

[[nodiscard]] Digest chain_of(const Digest& previous,
                              Sequence sequence,
                              RecordType type,
                              std::span<const std::uint8_t> payload) {
    Sha256 hasher;
    hasher.update(Bytes{previous.bytes().data(), previous.bytes().size()});
    std::uint8_t scratch[24] = {};
    put_u64(scratch, sequence.value());
    put_u32(scratch + 8, static_cast<std::uint32_t>(type));
    put_u32(scratch + 12, static_cast<std::uint32_t>(payload.size()));
    put_u32(scratch + 16, 0);
    put_u32(scratch + 20, 0);
    hasher.update(Bytes{scratch, sizeof(scratch)});
    hasher.update(payload);
    return Digest{hasher.finish()};
}

struct FrameInfo {
    RecordType type = RecordType::Note;
    Sequence sequence;
    std::size_t offset = 0;
    std::size_t payload_length = 0;
    Digest chain;
};

// Validates one frame in place. Returns false and describes why when the frame
// is not intact; the caller decides whether that is a recoverable tail.
[[nodiscard]] bool parse_frame(std::span<const std::uint8_t> bytes,
                               std::size_t offset,
                               std::size_t& frame_bytes,
                               FrameInfo& info,
                               std::string& reason) {
    if (bytes.size() - offset < kHeaderBytes) {
        reason = "incomplete record header";
        return false;
    }
    const std::uint8_t* header = bytes.data() + offset;
    if (get_u32(header + kOffsetMagic) != kMagic) {
        reason = "record magic mismatch";
        return false;
    }
    if (get_u16(header + kOffsetVersion) != kFormatVersion) {
        reason = "record format version is not supported";
        return false;
    }
    if (get_u16(header + kOffsetFlags) != kFlagNone) {
        reason = "record flags are not zero";
        return false;
    }
    if (get_u32(header + kOffsetHeaderSize) != kHeaderBytes) {
        reason = "record header size mismatch";
        return false;
    }
    const std::uint32_t payload_length = get_u32(header + kOffsetPayloadLength);
    if (payload_length > kMaxPayloadBytes) {
        reason = "record payload length exceeds the accepted maximum";
        return false;
    }
    std::uint8_t copy[kHeaderBytes];
    std::memcpy(copy, header, kHeaderBytes);
    const std::uint32_t stored_header_crc = get_u32(header + kOffsetHeaderCrc);
    put_u32(copy + kOffsetHeaderCrc, 0);
    if (crc32c(Bytes{copy, kHeaderBytes}) != stored_header_crc) {
        reason = "record header checksum mismatch";
        return false;
    }
    if (get_u32(header + kOffsetReserved) != 0 || get_u64(header + kOffsetReserved2) != 0) {
        reason = "record reserved bytes are not zero";
        return false;
    }
    if (bytes.size() - offset < kHeaderBytes + payload_length) {
        reason = "record payload is truncated";
        return false;
    }
    const std::span<const std::uint8_t> payload{bytes.data() + offset + kHeaderBytes,
                                                payload_length};
    if (crc32c(payload) != get_u32(header + kOffsetBodyCrc)) {
        reason = "record body checksum mismatch";
        return false;
    }
    const std::uint32_t raw_type = get_u32(header + kOffsetType);
    if (!is_known_record_type(raw_type)) {
        reason = "record type is not known";
        return false;
    }
    Sha256::DigestBytes digest_bytes{};
    std::memcpy(digest_bytes.data(), header + kOffsetChain, digest_bytes.size());
    info.type = static_cast<RecordType>(raw_type);
    info.sequence = Sequence{get_u64(header + kOffsetSequence)};
    info.offset = offset;
    info.payload_length = payload_length;
    info.chain = Digest{digest_bytes};
    frame_bytes = kHeaderBytes + payload_length;
    return true;
}

[[nodiscard]] bool scan_window_has_valid_frame(std::span<const std::uint8_t> bytes,
                                               std::size_t from) {
    // Resynchronisation probe: if an intact frame can be found after the damage,
    // the damage is interior and must not be truncated away.
    for (std::size_t offset = from; offset + kHeaderBytes <= bytes.size(); ++offset) {
        if (get_u32(bytes.data() + offset + kOffsetMagic) != kMagic) {
            continue;
        }
        std::size_t frame_bytes = 0;
        FrameInfo info;
        std::string reason;
        if (parse_frame(bytes, offset, frame_bytes, info, reason)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
    switch (type) {
        case RecordType::Header: return "header";
        case RecordType::Site: return "site";
        case RecordType::FailureDomain: return "failure_domain";
        case RecordType::Obligation: return "obligation";
        case RecordType::Policy: return "policy";
        case RecordType::Capability: return "capability";
        case RecordType::Readiness: return "readiness";
        case RecordType::Federation: return "federation";
        case RecordType::EventDeclaration: return "event_declaration";
        case RecordType::EventAssessment: return "event_assessment";
        case RecordType::EventTransition: return "event_transition";
        case RecordType::Plan: return "plan";
        case RecordType::StepTransition: return "step_transition";
        case RecordType::Dispatch: return "dispatch";
        case RecordType::Receipt: return "receipt";
        case RecordType::RejectedEvidence: return "rejected_evidence";
        case RecordType::ReturnToService: return "return_to_service";
        case RecordType::FailbackAuthorization: return "failback_authorization";
        case RecordType::SnapshotRef: return "snapshot_ref";
        case RecordType::Commit: return "commit";
        case RecordType::Note: return "note";
        case RecordType::Endpoint: return "endpoint";
    }
    return "unknown";
}

bool is_known_record_type(std::uint32_t raw) noexcept {
    return raw >= static_cast<std::uint32_t>(RecordType::Header) &&
           raw <= static_cast<std::uint32_t>(RecordType::Endpoint);
}

std::vector<std::uint8_t> encode_frame(RecordType type,
                                       Sequence sequence,
                                       const Digest& previous_chain,
                                       std::span<const std::uint8_t> payload,
                                       Digest* out_chain) {
    if (payload.size() > kMaxPayloadBytes) {
        return {};
    }
    const Digest chain = chain_of(previous_chain, sequence, type, payload);
    std::vector<std::uint8_t> frame(kHeaderBytes + payload.size(), 0);
    put_u32(frame.data() + kOffsetMagic, kMagic);
    put_u16(frame.data() + kOffsetVersion, kFormatVersion);
    put_u16(frame.data() + kOffsetFlags, kFlagNone);
    put_u32(frame.data() + kOffsetHeaderSize, static_cast<std::uint32_t>(kHeaderBytes));
    put_u32(frame.data() + kOffsetPayloadLength, static_cast<std::uint32_t>(payload.size()));
    put_u64(frame.data() + kOffsetSequence, sequence.value());
    put_u32(frame.data() + kOffsetType, static_cast<std::uint32_t>(type));
    put_u32(frame.data() + kOffsetBodyCrc, crc32c(payload));
    std::memcpy(frame.data() + kOffsetChain, chain.bytes().data(), chain.bytes().size());
    std::memcpy(frame.data() + kHeaderBytes, payload.data(), payload.size());
    put_u32(frame.data() + kOffsetHeaderCrc, 0);
    put_u32(frame.data() + kOffsetHeaderCrc,
            crc32c(Bytes{frame.data(), kHeaderBytes}));
    if (out_chain != nullptr) {
        *out_chain = chain;
    }
    return frame;
}

std::vector<std::uint8_t> encode_commit(const CommitPayload& payload) {
    canonical::Writer writer;
    writer.u64(payload.committed_through.value());
    writer.u64(payload.record_count);
    const Sha256::DigestBytes& digest = payload.state_digest.bytes();
    writer.blob(std::span<const std::uint8_t>(digest.data(), digest.size()));
    writer.u64(payload.epoch.value());
    writer.i64(payload.committed_at);
    return writer.take();
}

Result<CommitPayload> decode_commit(std::span<const std::uint8_t> bytes) {
    canonical::Reader reader(bytes);
    CommitPayload payload;
    payload.committed_through = Sequence{reader.u64()};
    payload.record_count = reader.u64();
    const std::span<const std::uint8_t> digest = reader.blob();
    if (digest.size() != 32) {
        return Status{ErrorCode::Invalid, "commit digest is not 32 bytes"};
    }
    Sha256::DigestBytes digest_bytes{};
    for (std::size_t i = 0; i < digest_bytes.size(); ++i) {
        digest_bytes[i] = digest[i];
    }
    payload.state_digest = Digest{digest_bytes};
    payload.epoch = Epoch{reader.u64()};
    payload.committed_at = reader.i64();
    if (reader.failed()) {
        return reader.status();
    }
    if (!reader.at_end()) {
        return Status{ErrorCode::Invalid, "trailing bytes in commit record"};
    }
    return payload;
}

std::vector<std::uint8_t> encode_snapshot_ref(const SnapshotRefPayload& payload) {
    canonical::Writer writer;
    writer.u64(payload.covered_through.value());
    writer.u64(payload.snapshot_epoch.value());
    writer.u64(payload.payload_bytes);
    const Sha256::DigestBytes& state = payload.state_digest.bytes();
    writer.blob(std::span<const std::uint8_t>(state.data(), state.size()));
    const Sha256::DigestBytes& payload_digest = payload.payload_digest.bytes();
    writer.blob(std::span<const std::uint8_t>(payload_digest.data(), payload_digest.size()));
    writer.i64(payload.written_at);
    return writer.take();
}

Result<SnapshotRefPayload> decode_snapshot_ref(std::span<const std::uint8_t> bytes) {
    canonical::Reader reader(bytes);
    SnapshotRefPayload payload;
    payload.covered_through = Sequence{reader.u64()};
    payload.snapshot_epoch = Epoch{reader.u64()};
    payload.payload_bytes = reader.u64();
    const std::span<const std::uint8_t> state = reader.blob();
    const std::span<const std::uint8_t> digest = reader.blob();
    if (state.size() != 32 || digest.size() != 32) {
        return Status{ErrorCode::Invalid, "snapshot reference digest is not 32 bytes"};
    }
    Sha256::DigestBytes state_bytes{};
    Sha256::DigestBytes payload_bytes{};
    for (std::size_t i = 0; i < state_bytes.size(); ++i) {
        state_bytes[i] = state[i];
        payload_bytes[i] = digest[i];
    }
    payload.state_digest = Digest{state_bytes};
    payload.payload_digest = Digest{payload_bytes};
    payload.written_at = reader.i64();
    if (reader.failed()) {
        return reader.status();
    }
    if (!reader.at_end()) {
        return Status{ErrorCode::Invalid, "trailing bytes in snapshot reference"};
    }
    return payload;
}

struct Reader::Impl {
    std::vector<std::uint8_t> bytes;
    std::vector<FrameInfo> frames;
};

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Reader::~Reader() = default;

Result<Reader> Reader::open(const std::string& path, std::uint64_t max_bytes) {
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, max_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    Reader reader;
    reader.impl_ = std::make_unique<Impl>();
    reader.impl_->bytes = std::move(bytes).value();
    ScanReport& report = reader.report_;
    report.file_bytes = reader.impl_->bytes.size();
    if (reader.impl_->bytes.empty()) {
        report.empty = true;
        report.last_record_sequence = Sequence{};
        report.last_committed_sequence = Sequence{};
        return reader;
    }

    const std::span<const std::uint8_t> all{reader.impl_->bytes.data(),
                                           reader.impl_->bytes.size()};
    std::size_t offset = 0;
    Sequence expected{1};
    Digest previous{};
    std::size_t committed_bytes = 0;
    Sequence last_committed_sequence{};
    std::uint64_t committed_records = 0;
    bool saw_header = false;

    while (offset < all.size()) {
        std::size_t frame_bytes = 0;
        FrameInfo info;
        std::string reason;
        if (!parse_frame(all, offset, frame_bytes, info, reason)) {
            const bool in_uncommitted_region = offset >= committed_bytes;
            const bool valid_frame_after = scan_window_has_valid_frame(all, offset + 1);
            if (in_uncommitted_region && !valid_frame_after) {
                report.torn_tail = true;
                report.detail = reason;
            } else {
                report.interior_corruption = true;
                report.corruption_offset = offset;
                report.detail = reason;
            }
            break;
        }
        if (info.sequence != expected) {
            report.interior_corruption = true;
            report.corruption_offset = offset;
            report.detail = "record sequence is not contiguous";
            break;
        }
        const Digest expected_chain = chain_of(previous, info.sequence, info.type,
                                               std::span<const std::uint8_t>(
                                                   all.data() + offset + kHeaderBytes,
                                                   info.payload_length));
        if (expected_chain != info.chain) {
            report.interior_corruption = true;
            report.corruption_offset = offset;
            report.detail = "record chain does not match the previous record";
            break;
        }
        if (info.type == RecordType::Header && !saw_header && info.sequence.value() == 1) {
            saw_header = true;
            const std::span<const std::uint8_t> payload{all.data() + offset + kHeaderBytes,
                                                        info.payload_length};
            Result<codec::JournalHeader> header = codec::decode_journal_header(payload);
            if (header.ok()) {
                report.recorded_epoch = header.value().epoch;
            }
        }
        reader.impl_->frames.push_back(info);
        previous = info.chain;
        report.record_count += 1;
        report.last_record_sequence = info.sequence;
        if (info.type == RecordType::Commit) {
            committed_bytes = offset + frame_bytes;
            last_committed_sequence = info.sequence;
            committed_records = report.record_count;
            report.chain = info.chain;
        } else if (committed_bytes == 0 && info.type == RecordType::Header) {
            // Before the first commit exists, the header record is the only
            // durable content of a freshly created journal. Its sequence and
            // chain are the committed boundary: without them a writer that
            // truncated an uncommitted tail here would resume from the sequence
            // of the record it just discarded and write a gap.
            committed_bytes = offset + frame_bytes;
            committed_records = report.record_count;
            last_committed_sequence = info.sequence;
            report.chain = info.chain;
        }
        offset += frame_bytes;
        Sequence next = expected.next().ok() ? expected.next().value() : Sequence{};
        if (!next.valid()) {
            report.interior_corruption = true;
            report.corruption_offset = offset;
            report.detail = "record sequence exhausted";
            break;
        }
        expected = next;
    }

    if (!report.interior_corruption && report.torn_tail) {
        // A tail that is damaged but sits entirely after the last commit is
        // recoverable: the committed prefix is intact and self-consistent.
        report.committed_bytes = committed_bytes;
        report.discarded_tail_bytes = report.file_bytes - committed_bytes;
    } else if (!report.interior_corruption) {
        report.committed_bytes = committed_bytes;
        report.discarded_tail_bytes = report.file_bytes - committed_bytes;
        if (report.discarded_tail_bytes == 0) {
            report.chain = previous;
        }
    }
    report.committed_record_count = committed_records;
    report.last_committed_sequence = last_committed_sequence;
    if (!report.interior_corruption && committed_bytes == 0 && report.record_count > 0) {
        report.interior_corruption = true;
        report.corruption_offset = 0;
        report.detail = "journal has no committed boundary";
    }
    return reader;
}

Status Reader::for_each_committed(const std::function<Status(const Record&)>& visitor) const {
    if (report_.interior_corruption) {
        return Status{ErrorCode::Corrupt, "journal has interior corruption; replay refused"};
    }
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "journal reader is not open"};
    }
    const std::span<const std::uint8_t> all{impl_->bytes.data(), impl_->bytes.size()};
    for (const FrameInfo& frame : impl_->frames) {
        if (frame.offset + kHeaderBytes + frame.payload_length > report_.committed_bytes) {
            break;
        }
        Record record;
        record.type = frame.type;
        record.sequence = frame.sequence;
        record.payload.assign(all.data() + frame.offset + kHeaderBytes,
                              all.data() + frame.offset + kHeaderBytes + frame.payload_length);
        const Status status = visitor(record);
        if (!status.ok()) {
            return status;
        }
    }
    return ok_status();
}

struct Writer::Impl {
    fileio::File file;
    std::vector<std::uint8_t> pending;
};

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

Writer::~Writer() = default;

Result<Writer> Writer::open(const Options& options) {
    Writer writer;
    writer.options_ = options;
    if (options.max_bytes == 0) {
        return Status{ErrorCode::Invalid, "journal byte bound must be positive"};
    }

    Result<bool> present = fileio::exists(options.path);
    if (!present.ok()) {
        return present.status();
    }
    if (present.value()) {
        Result<Reader> reader = Reader::open(options.path, options.max_bytes);
        if (!reader.ok()) {
            return reader.status();
        }
        writer.report_ = reader.value().report();
        if (writer.report_.interior_corruption) {
            std::string message = "journal is corrupt at offset ";
            message.append(std::to_string(writer.report_.corruption_offset));
            message.append(": ");
            message.append(writer.report_.detail);
            return Status{ErrorCode::Corrupt, std::move(message)};
        }
        if (writer.report_.empty) {
            writer.sequence_ = Sequence{};
            writer.chain_ = Digest{};
        } else {
            writer.sequence_ = writer.report_.last_record_sequence;
            writer.chain_ = writer.report_.chain;
            writer.bytes_written_ = writer.report_.file_bytes;
        }
    }

    Result<fileio::File> file = fileio::File::open(options.path, "ab");
    if (!file.ok()) {
        return file.status();
    }
    writer.impl_ = std::make_unique<Impl>();
    writer.impl_->file = std::move(file).value();

    if (writer.report_.discarded_tail_bytes > 0 && options.truncate_to_committed) {
        const Status truncated = writer.impl_->file.truncate(writer.report_.committed_bytes);
        if (!truncated.ok()) {
            return truncated;
        }
        const Status flushed = writer.impl_->file.flush_data();
        if (!flushed.ok()) {
            return flushed;
        }
        // The magic and version framing are unchanged, so the chain of the last
        // committed record is the writer's chain.
        writer.sequence_ = writer.report_.last_committed_sequence.valid()
                               ? writer.report_.last_committed_sequence
                               : writer.report_.last_record_sequence;
        writer.chain_ = writer.report_.chain;
        writer.bytes_written_ = writer.report_.committed_bytes;
        if (writer.report_.committed_record_count == 0) {
            writer.sequence_ = Sequence{};
            writer.chain_ = Digest{};
        }
    }
    return writer;
}

Result<Sequence> Writer::append(RecordType type, std::span<const std::uint8_t> payload) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "journal writer is not open"};
    }
    if (payload.empty()) {
        return Status{ErrorCode::Invalid, "refusing to append an empty record payload"};
    }
    if (payload.size() > kMaxPayloadBytes) {
        return Status{ErrorCode::Exhausted, "record payload exceeds the accepted maximum"};
    }
    Result<Sequence> next = sequence_.next();
    if (!next.ok()) {
        return next.status();
    }
    if (bytes_written_ + impl_->pending.size() + kHeaderBytes + payload.size() > options_.max_bytes) {
        return Status{ErrorCode::Exhausted,
                      "journal would exceed its configured maximum size; compact it first"};
    }
    Digest new_chain;
    const std::vector<std::uint8_t> frame =
        encode_frame(type, next.value(), chain_, payload, &new_chain);
    if (frame.empty()) {
        return Status{ErrorCode::Exhausted, "record frame could not be encoded"};
    }
    impl_->pending.insert(impl_->pending.end(), frame.begin(), frame.end());
    sequence_ = next.value();
    chain_ = new_chain;
    return sequence_;
}

Status Writer::flush() {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "journal writer is not open"};
    }
    if (impl_->pending.empty()) {
        return ok_status();
    }
    const Status written = impl_->file.write(impl_->pending);
    if (!written.ok()) {
        return written;
    }
    bytes_written_ += impl_->pending.size();
    impl_->pending.clear();
    // Durability boundary: everything reported as appended is on stable
    // storage once this returns.
    return impl_->file.flush_data();
}

Status Writer::commit(const CommitPayload& payload) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "journal writer is not open"};
    }
    const Status flushed = flush();
    if (!flushed.ok()) {
        return flushed;
    }
    Result<Sequence> next = sequence_.next();
    if (!next.ok()) {
        return next.status();
    }
    CommitPayload effective = payload;
    effective.committed_through = sequence_;
    Digest new_chain;
    const std::vector<std::uint8_t> encoded = encode_commit(effective);
    const std::vector<std::uint8_t> frame =
        encode_frame(RecordType::Commit, next.value(), chain_, encoded, &new_chain);
    if (frame.empty()) {
        return Status{ErrorCode::Exhausted, "commit frame could not be encoded"};
    }
    const Status written = impl_->file.write(frame);
    if (!written.ok()) {
        return written;
    }
    const Status durable = impl_->file.flush_data();
    if (!durable.ok()) {
        return durable;
    }
    bytes_written_ += frame.size();
    sequence_ = next.value();
    chain_ = new_chain;
    commits_ += 1;
    report_.committed_bytes = bytes_written_;
    report_.last_committed_sequence = sequence_;
    return ok_status();
}

Status Writer::rewrite(const std::vector<Record>& records, const CommitPayload& final_commit) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "journal writer is not open"};
    }
    std::vector<std::uint8_t> rebuilt;
    Digest chain{};
    Sequence sequence{};
    std::uint64_t total = 0;
    for (const Record& record : records) {
        Result<Sequence> next = sequence.next();
        if (!next.ok()) {
            return next.status();
        }
        Digest new_chain;
        const std::vector<std::uint8_t> frame =
            encode_frame(record.type, next.value(), chain, record.payload, &new_chain);
        if (frame.empty()) {
            return Status{ErrorCode::Exhausted, "record frame could not be encoded"};
        }
        total += frame.size();
        if (total > options_.max_bytes) {
            return Status{ErrorCode::Exhausted, "rewritten journal would exceed its maximum size"};
        }
        rebuilt.insert(rebuilt.end(), frame.begin(), frame.end());
        chain = new_chain;
        sequence = next.value();
    }
    CommitPayload effective = final_commit;
    effective.committed_through = sequence;
    Result<Sequence> commit_sequence = sequence.next();
    if (!commit_sequence.ok()) {
        return commit_sequence.status();
    }
    Digest commit_chain;
    const std::vector<std::uint8_t> encoded_commit = encode_commit(effective);
    const std::vector<std::uint8_t> commit_frame =
        encode_frame(RecordType::Commit, commit_sequence.value(), chain, encoded_commit,
                     &commit_chain);
    if (commit_frame.empty()) {
        return Status{ErrorCode::Exhausted, "commit frame could not be encoded"};
    }
    rebuilt.insert(rebuilt.end(), commit_frame.begin(), commit_frame.end());

    // The open handle must be released before the file can be replaced on
    // Windows; the new file is reopened afterwards either way.
    Status closed = impl_->file.close();
    if (!closed.ok()) {
        return closed;
    }
    Status failure = ok_status();
    const std::string temporary = options_.path + ".rewrite";
    const Status written =
        fileio::write_file_durable(temporary, std::span<const std::uint8_t>(rebuilt));
    if (!written.ok()) {
        failure = written;
    } else {
        const Status replaced = fileio::atomic_replace(temporary, options_.path);
        if (!replaced.ok()) {
            (void)fileio::remove_file(temporary);
            failure = replaced;
        } else {
            const std::size_t separator = options_.path.find_last_of("/\\");
            const std::string directory = separator == std::string::npos
                                              ? std::string{"."}
                                              : options_.path.substr(0, separator);
            failure = fileio::sync_directory(directory);
        }
    }
    Result<fileio::File> reopened = fileio::File::open(options_.path, "ab");
    if (!reopened.ok()) {
        return failure.ok() ? reopened.status() : failure;
    }
    impl_->file = std::move(reopened).value();
    if (!failure.ok()) {
        return failure;
    }
    impl_->pending.clear();
    sequence_ = commit_sequence.value();
    chain_ = commit_chain;
    bytes_written_ = rebuilt.size();
    commits_ += 1;
    report_.committed_bytes = rebuilt.size();
    report_.last_committed_sequence = sequence_;
    report_.record_count = records.size() + 1;
    report_.committed_record_count = records.size() + 1;
    report_.file_bytes = rebuilt.size();
    report_.discarded_tail_bytes = 0;
    report_.torn_tail = false;
    report_.chain = chain_;
    return ok_status();
}

Status Writer::close() {
    if (impl_ == nullptr) {
        return ok_status();
    }
    const Status flushed = flush();
    const Status closed = impl_->file.close();
    impl_.reset();
    if (!flushed.ok()) {
        return flushed;
    }
    return closed;
}

}  // namespace drc::journal
