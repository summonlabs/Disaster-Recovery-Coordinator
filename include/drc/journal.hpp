#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "drc/digest.hpp"
#include "drc/error.hpp"
#include "drc/id.hpp"
#include "drc/time.hpp"

namespace drc::journal {

// Durable format. Both constants and layout are frozen: changing either is a
// format version bump plus a migration test, never an edit in place.
inline constexpr std::uint32_t kMagic = 0x4A435244u;  // 'DRCJ' little-endian
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderBytes = 80;
inline constexpr std::uint16_t kFlagNone = 0;

// Payload kinds. The payload of every record is the canonical encoding of the
// named object; the record type only names which decoder applies.
enum class RecordType : std::uint32_t {
    Header = 1,
    Site = 2,
    FailureDomain = 3,
    Obligation = 4,
    Policy = 5,
    Capability = 6,
    Readiness = 7,
    Federation = 8,
    EventDeclaration = 9,
    EventAssessment = 10,
    EventTransition = 11,
    Plan = 12,
    StepTransition = 13,
    Dispatch = 14,
    Receipt = 15,
    RejectedEvidence = 16,
    ReturnToService = 17,
    FailbackAuthorization = 18,
    SnapshotRef = 19,
    Commit = 20,
    Note = 21,
    Endpoint = 22,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;
[[nodiscard]] bool is_known_record_type(std::uint32_t raw) noexcept;

struct Record {
    RecordType type = RecordType::Note;
    Sequence sequence;
    std::vector<std::uint8_t> payload;
};

// The inner payload of a commit record. The durability boundary of every
// state-changing call is the flush of this record.
struct CommitPayload {
    Sequence committed_through;
    std::uint64_t record_count = 0;
    Digest state_digest;
    Epoch epoch;
    UnixNanos committed_at = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_commit(const CommitPayload& payload);
[[nodiscard]] Result<CommitPayload> decode_commit(std::span<const std::uint8_t> bytes);

struct SnapshotRefPayload {
    Sequence covered_through;
    Epoch snapshot_epoch;
    std::uint64_t payload_bytes = 0;
    Digest state_digest;
    Digest payload_digest;
    UnixNanos written_at = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_snapshot_ref(const SnapshotRefPayload& payload);
[[nodiscard]] Result<SnapshotRefPayload> decode_snapshot_ref(std::span<const std::uint8_t> bytes);

// Outcome of reading a journal from byte zero. It separates a recoverable
// uncommitted tail from damage that must not be papered over.
struct ScanReport {
    std::uint64_t file_bytes = 0;
    std::uint64_t committed_bytes = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t record_count = 0;
    std::uint64_t committed_record_count = 0;
    Sequence last_record_sequence;
    Sequence last_committed_sequence;
    Epoch recorded_epoch;
    Digest chain;
    bool torn_tail = false;
    bool interior_corruption = false;
    bool empty = false;
    std::uint64_t corruption_offset = 0;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return !interior_corruption; }
};

// Reads a journal file. The file is never modified by a reader.
class Reader {
public:
    Reader() = default;
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    // max_bytes bounds how much of the file is examined, so a pathological file
    // cannot exhaust memory or time.
    [[nodiscard]] static Result<Reader> open(const std::string& path, std::uint64_t max_bytes);

    [[nodiscard]] const ScanReport& report() const noexcept { return report_; }

    // Visits every committed record in sequence order. The callback returns a
    // Status; the first failure stops the walk and is returned.
    [[nodiscard]] Status for_each_committed(
        const std::function<Status(const Record&)>& visitor) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ScanReport report_{};
};

// Appends records and commits them. One writer per journal is enforced by the
// lock file, not by this class.
class Writer {
public:
    Writer() = default;
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    struct Options {
        std::string path;
        std::uint64_t max_bytes = 64ull * 1024ull * 1024ull;
        bool truncate_to_committed = true;
    };

    // Opens (creating when absent) and, when the tail is an uncommitted partial
    // record, truncates back to the last committed boundary. Interior
    // corruption fails the open and the file is left untouched.
    [[nodiscard]] static Result<Writer> open(const Options& options);

    [[nodiscard]] const ScanReport& scan() const noexcept { return report_; }

    // Buffered append. Returns the sequence assigned to the record.
    [[nodiscard]] Result<Sequence> append(RecordType type, std::span<const std::uint8_t> payload);

    // Flushes every appended record to the operating system and to stable
    // storage, then appends and flushes the commit record. This is the durable
    // boundary the API promises.
    [[nodiscard]] Status commit(const CommitPayload& payload);

    [[nodiscard]] Status flush();

    [[nodiscard]] Sequence last_sequence() const noexcept { return sequence_; }
    [[nodiscard]] Digest chain() const noexcept { return chain_; }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }
    [[nodiscard]] std::uint64_t commits() const noexcept { return commits_; }

    // Replaces the file with exactly these records, re-chained from scratch.
    // The replacement is durable before the old file is unlinked, and the
    // caller must only pass records at or below the committed boundary.
    [[nodiscard]] Status rewrite(const std::vector<Record>& records,
                                 const CommitPayload& final_commit);

    [[nodiscard]] const std::string& path() const noexcept { return options_.path; }
    [[nodiscard]] Status close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ScanReport report_{};
    Options options_{};
    Sequence sequence_;
    Digest chain_;
    std::uint64_t bytes_written_ = 0;
    std::uint64_t commits_ = 0;
};

// Bytes that a header occupies, exposed so tests can build deliberately
// damaged journals byte by byte.
[[nodiscard]] std::vector<std::uint8_t> encode_frame(RecordType type,
                                                     Sequence sequence,
                                                     const Digest& previous_chain,
                                                     std::span<const std::uint8_t> payload,
                                                     Digest* out_chain);

}  // namespace drc::journal
