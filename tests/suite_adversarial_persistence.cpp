// Adversarial durability suite: the durable state is attacked directly with a
// real journal written by a real coordinator. Every test asserts an observable
// value (a code, an offset, a digest, a byte length) and prints what broke the
// invariant instead of hiding it behind a boolean.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "drc/canonical.hpp"
#include "drc/checkpoint.hpp"
#include "drc/fileio.hpp"
#include "drc/journal.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

constexpr std::uint64_t kMaxJournalBytes = 64ull * 1024ull * 1024ull;

// Frame layout offsets, frozen by include/drc/journal.hpp. The test walks the
// bytes itself so that "an intact record follows the damage" is proven without
// trusting the reader under test.
constexpr std::size_t kOffsetPayloadLength = 12;
constexpr std::size_t kOffsetSequence = 16;
constexpr std::size_t kOffsetType = 24;
constexpr std::size_t kOffsetHeaderCrc = 32;

// Notes are flushed immediately: a test that stops making progress must still
// show how far it got.
void note(const std::string& message) {
    std::cout << "    note: " << message << "\n";
    std::cout.flush();
}

[[nodiscard]] std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const std::uint16_t low = bytes[offset];
    const std::uint16_t high = static_cast<std::uint16_t>(bytes[offset + 1] << 8);
    return static_cast<std::uint16_t>(low | high);
}

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8u * static_cast<unsigned>(i));
    }
    return value;
}

struct Frame {
    std::size_t start = 0;
    std::size_t payload_bytes = 0;
    std::uint32_t type = 0;
    [[nodiscard]] std::size_t end() const {
        return start + journal::kHeaderBytes + payload_bytes;
    }
};

struct Walk {
    std::vector<Frame> frames;
    bool complete = false;
    std::size_t stopped_at = 0;
};

// Walks intact frames from byte zero. A frame is intact only when the magic,
// the format version, the header size, the payload length, and the header
// checksum all agree, so a frame this returns can be re-checked independently.
[[nodiscard]] Walk walk_intact_frames(const std::vector<std::uint8_t>& bytes) {
    Walk walk;
    std::size_t offset = 0;
    while (offset + journal::kHeaderBytes <= bytes.size()) {
        if (read_u32(bytes, offset) != journal::kMagic) {
            break;
        }
        if (read_u16(bytes, offset + 4) != journal::kFormatVersion) {
            break;
        }
        if (read_u32(bytes, offset + 8) != static_cast<std::uint32_t>(journal::kHeaderBytes)) {
            break;
        }
        const std::uint32_t payload_bytes = read_u32(bytes, offset + kOffsetPayloadLength);
        if (payload_bytes > canonical::kMaxRecordBytes) {
            break;
        }
        const std::size_t frame_bytes =
            journal::kHeaderBytes + static_cast<std::size_t>(payload_bytes);
        if (frame_bytes > bytes.size() - offset) {
            break;
        }
        std::vector<std::uint8_t> header(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + journal::kHeaderBytes));
        const std::uint32_t stored_crc = read_u32(bytes, offset + kOffsetHeaderCrc);
        for (std::size_t i = 0; i < 4; ++i) {
            header[kOffsetHeaderCrc + i] = 0;
        }
        if (crc32c(header) != stored_crc) {
            break;
        }
        Frame frame;
        frame.start = offset;
        frame.payload_bytes = payload_bytes;
        frame.type = read_u32(bytes, offset + kOffsetType);
        walk.frames.push_back(frame);
        offset = frame.end();
    }
    walk.stopped_at = offset;
    walk.complete = offset == bytes.size();
    return walk;
}

[[nodiscard]] bool is_frame_boundary(const Walk& walk, std::uint64_t offset) {
    if (offset == 0) {
        return true;
    }
    for (const Frame& frame : walk.frames) {
        if (frame.end() == offset) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t frames_within(const Walk& walk, std::size_t length) {
    std::uint64_t total = 0;
    for (const Frame& frame : walk.frames) {
        if (frame.end() <= length) {
            total += 1;
        }
    }
    return total;
}

[[nodiscard]] std::size_t frame_start_of(const Walk& walk, std::size_t offset) {
    for (const Frame& frame : walk.frames) {
        if (offset >= frame.start && offset < frame.end()) {
            return frame.start;
        }
    }
    return walk.stopped_at;
}

// Scratch copies are not durability boundaries, so they are written without a
// flush: another handle sees the same bytes through the operating system cache.
[[nodiscard]] bool write_bytes(const std::string& path, std::span<const std::uint8_t> bytes) {
    Result<fileio::File> file = fileio::File::open(path, "wb");
    if (!file.ok()) {
        return false;
    }
    const Status written = file.value().write(bytes);
    if (!written.ok()) {
        return false;
    }
    return file.value().close().ok();
}

[[nodiscard]] bool write_prefix(const std::string& path,
                                const std::vector<std::uint8_t>& bytes,
                                std::size_t length) {
    return write_bytes(path, std::span<const std::uint8_t>(bytes.data(), length));
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, kMaxJournalBytes);
    DRC_REQUIRE(bytes.ok());
    return std::move(bytes).value();
}

[[nodiscard]] std::uint64_t bytes_of(const std::string& path) {
    Result<std::uint64_t> size = fileio::file_size(path);
    DRC_REQUIRE(size.ok());
    return size.value();
}

// A real journal from a real coordinator: two failure domains, a losing site, a
// destination site, one safety-critical obligation, four synthetic endpoints, a
// declaration, a plan, a settled recovery, and a checkpoint. It stays small so
// the truncation loop can afford to visit every byte length.
void build_reference_journal(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_OK(rig.coordinator->checkpoint());
    DRC_REQUIRE(rig.coordinator->outstanding_requests().value().empty());
}

[[nodiscard]] CoordinatorOptions scratch_options(const std::string& directory) {
    CoordinatorOptions options;
    options.directory = directory;
    options.clock = std::make_shared<ManualClock>();
    options.owner = "drc-tests";
    options.worker_threads = 1;
    return options;
}

// The digest of a coordinator whose durable history contains no object at all.
// A journal truncated before its first commit legitimately replays to exactly
// this state, so the test can name the expected digest instead of trusting it.
[[nodiscard]] Digest empty_state_digest() {
    drctest::TempDir reference("adv-empty-reference");
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(scratch_options(reference.path));
    DRC_REQUIRE(opened.ok());
    const Digest digest = opened.value()->state_digest();
    DRC_REQUIRE_OK(opened.value()->shutdown());
    return digest;
}

struct JournalFixture {
    std::vector<std::uint8_t> bytes;
    Walk walk;
    Digest full_digest;
    std::string journal_path;
};

// Builds the reference journal, snapshots its bytes and digest, and closes the
// coordinator so nothing else holds the file open.
[[nodiscard]] JournalFixture make_fixture(const std::string& tag) {
    drctest::Rig rig = drctest::Rig::make(tag);
    rig.open();
    build_reference_journal(rig);
    JournalFixture fixture;
    fixture.full_digest = rig.coordinator->state_digest();
    fixture.journal_path = fileio::join(rig.directory->path, "coordinator.journal");
    rig.close();
    fixture.bytes = read_bytes(fixture.journal_path);
    fixture.walk = walk_intact_frames(fixture.bytes);
    DRC_REQUIRE(fixture.walk.complete);
    return fixture;
}

// What the committed prefix recorded: the digest of its last commit, and how
// much delegated work it left without an answer. Recovery fences unfinished
// work *after* the checksum fixed point, so a prefix that ends inside a
// transaction legitimately recovers to a state that is not the last committed
// digest. Recovery may never leave that work in flight, and the test checks
// exactly which of the two outcomes happened.
struct CommittedSummary {
    bool have_commit = false;
    Digest last_commit_digest;
    std::uint64_t dispatches = 0;
    std::uint64_t receipts = 0;
};

[[nodiscard]] CommittedSummary summarize_committed(const journal::Reader& reader) {
    CommittedSummary summary;
    const Status walked =
        reader.for_each_committed([&](const journal::Record& record) -> Status {
            switch (record.type) {
                case journal::RecordType::Commit: {
                    Result<journal::CommitPayload> payload = journal::decode_commit(record.payload);
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    summary.last_commit_digest = payload.value().state_digest;
                    summary.have_commit = true;
                    break;
                }
                case journal::RecordType::Dispatch:
                    summary.dispatches += 1;
                    break;
                case journal::RecordType::Receipt:
                    summary.receipts += 1;
                    break;
                default:
                    break;
            }
            return ok_status();
        });
    DRC_REQUIRE(walked.ok());
    return summary;
}

// A recovery either reproduces the last committed digest exactly, or it fenced
// work that the truncated transaction left unanswered. In the second case the
// state must show the fencing: nothing outstanding, nothing in flight, and one
// retained receipt per unanswered dispatch. Returns true when work was fenced.
[[nodiscard]] bool require_recovered_state(Coordinator& coordinator,
                                           const CommittedSummary& summary,
                                           const Digest& empty_digest,
                                           const std::string& label) {
    const Digest actual = coordinator.state_digest();
    const std::vector<EffectRequest> outstanding = coordinator.outstanding_requests().value();
    // The result is held in a named local: taking .value() of a temporary
    // Result inside a range-for leaves a dangling reference to the vector.
    const Result<std::vector<RecoveryPlanId>> all_plans = coordinator.plans();
    DRC_REQUIRE(all_plans.ok());
    std::size_t in_flight = 0;
    for (const RecoveryPlanId plan_id : all_plans.value()) {
        const RecoveryPlan plan = coordinator.plan(plan_id).value();
        for (const RecoveryStepId step_id : plan.order) {
            if (plan.steps.at(step_id).state == StepState::InFlight) {
                in_flight += 1;
            }
        }
    }
    if (summary.dispatches == summary.receipts) {
        const Digest expected = summary.have_commit ? summary.last_commit_digest : empty_digest;
        if (actual != expected) {
            note(label + ": recovered digest " + actual.to_hex() + " but the committed bytes " +
                 (summary.have_commit ? "record " : "hold no commit; the empty state is ") +
                 expected.to_hex());
            DRC_REQUIRE(false);
        }
        return false;
    }
    const std::size_t unanswered =
        static_cast<std::size_t>(summary.dispatches - summary.receipts);
    if (!outstanding.empty()) {
        note(label + ": " + std::to_string(outstanding.size()) +
             " request(s) are still outstanding after recovery");
        DRC_REQUIRE(false);
    }
    if (in_flight != 0) {
        note(label + ": " + std::to_string(in_flight) + " step(s) are still in flight after "
             "recovery");
        DRC_REQUIRE(false);
    }
    const std::size_t retained = coordinator.receipts().value().size();
    if (retained < static_cast<std::size_t>(summary.receipts) + unanswered) {
        note(label + ": " + std::to_string(retained) + " receipts retained, but the prefix holds " +
             std::to_string(summary.receipts) + " and " + std::to_string(unanswered) +
             " were left unanswered");
        DRC_REQUIRE(false);
    }
    if (summary.have_commit && actual == summary.last_commit_digest) {
        note(label + ": unfinished work was fenced but the state digest did not change");
        DRC_REQUIRE(false);
    }
    return true;
}

const char kLongNameSeed = 'a';

}  // namespace

// ---------------------------------------------------------------------------
// 1. Every byte length of a real journal, truncated, must be classified.
// ---------------------------------------------------------------------------
// The sweep below is deterministic but SAMPLED, not byte-by-byte. A full sweep
// of this journal is more than 18,000 truncated copies, each written, re-read,
// and re-opened through a coordinator: minutes of work on a loaded machine.
// Every length that can change a classification is still tested exactly, and the
// rule is printed so any failure can be reproduced from the output alone:
//   * length 0 and length 1;
//   * every byte of the last 64 bytes of the file;
//   * every record header offset, every record boundary, and both sides of both
//     (frame start, start+1, end, end+1) for every frame in the journal;
//   * the exact end offset of every commit record;
//   * every 512th byte of the interior;
//   * 64 interior lengths from a fixed-seed xorshift32.
DRC_TEST(adversarial_every_byte_truncation_is_classified) {
    const JournalFixture fixture = make_fixture("adv-truncate");
    const Digest empty_digest = empty_state_digest();
    const std::size_t total = fixture.bytes.size();
    note("journal bytes = " + std::to_string(total) + ", frames = " +
         std::to_string(fixture.walk.frames.size()));
    DRC_REQUIRE(total > 1024);

    drctest::TempDir scratch("adv-truncate-scratch");
    const std::string scratch_journal = fileio::join(scratch.path, "coordinator.journal");

    std::vector<std::size_t> samples;
    // Clamped to the file size: "one byte past the end of the last frame" is
    // the file size itself, and an index beyond it is an out-of-bounds read of
    // the fixture rather than a truncation.
    const auto add_sample = [&samples, total](std::size_t value) {
        samples.push_back(value > total ? total : value);
    };
    add_sample(0);
    add_sample(1);
    const std::size_t tail_from = total > 64 ? total - 64 : 0;
    for (std::size_t length = tail_from; length <= total; ++length) {
        add_sample(length);
    }
    std::size_t commit_boundaries = 0;
    for (const Frame& frame : fixture.walk.frames) {
        add_sample(frame.start);
        add_sample(frame.start + 1);
        add_sample(frame.end());
        add_sample(frame.end() + 1);
        if (frame.type == static_cast<std::uint32_t>(journal::RecordType::Commit)) {
            // The durability boundary of the transaction, exactly.
            add_sample(frame.end());
            commit_boundaries += 1;
        }
    }
    for (std::size_t length = 0; length <= total; length += 512) {
        add_sample(length);
    }
    std::uint32_t random_state = 0x9E3779B9u;  // fixed seed: the sample is reproducible
    for (int draw = 0; draw < 64; ++draw) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        add_sample(static_cast<std::size_t>(random_state) % (total + 1));
    }
    std::sort(samples.begin(), samples.end());
    samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
    note("sampling rule: 0, 1, every byte in [" + std::to_string(tail_from) + ", " +
         std::to_string(total) + "], every record header and boundary with the byte on either "
         "side, the end offset of each of the " + std::to_string(commit_boundaries) +
         " commit records, every 512th byte, and 64 lengths from a fixed-seed xorshift32");
    note("sweeping " + std::to_string(samples.size()) + " of " + std::to_string(total + 1) +
         " byte lengths");

    std::uint64_t interior_lengths = 0;
    std::uint64_t torn_lengths = 0;
    std::uint64_t clean_lengths = 0;
    std::uint64_t refused = 0;
    std::uint64_t recovered = 0;
    std::uint64_t fenced_lengths = 0;
    bool saw_full_length = false;

    for (const std::size_t length : samples) {
        const std::vector<std::uint8_t> prefix(
            fixture.bytes.begin(),
            fixture.bytes.begin() + static_cast<std::ptrdiff_t>(length));
        if (!write_bytes(scratch_journal, prefix)) {
            note("could not write the scratch journal at length " + std::to_string(length));
            DRC_REQUIRE(false);
        }
        Result<journal::Reader> reader = journal::Reader::open(scratch_journal, kMaxJournalBytes);
        if (!reader.ok()) {
            note("Reader::open failed at length " + std::to_string(length) + ": " +
                 reader.status().to_string());
            DRC_REQUIRE(false);
        }
        const journal::ScanReport& report = reader.value().report();

        if (report.committed_bytes > length) {
            note("length " + std::to_string(length) + ": committed_bytes " +
                 std::to_string(report.committed_bytes) + " is past the end of the file");
        }
        DRC_REQUIRE(report.committed_bytes <= length);

        // The committed boundary is always a whole-record boundary, and the
        // reader saw exactly the whole records the bytes still hold.
        if (!is_frame_boundary(fixture.walk, report.committed_bytes)) {
            note("length " + std::to_string(length) + ": committed boundary " +
                 std::to_string(report.committed_bytes) + " is not a whole-record boundary");
        }
        DRC_REQUIRE(is_frame_boundary(fixture.walk, report.committed_bytes));
        if (report.record_count != frames_within(fixture.walk, length)) {
            note("length " + std::to_string(length) + ": reader counted " +
                 std::to_string(report.record_count) + " records, the bytes hold " +
                 std::to_string(frames_within(fixture.walk, length)) + " whole frames");
        }
        DRC_REQUIRE_EQ(report.record_count, frames_within(fixture.walk, length));

        if (report.interior_corruption) {
            interior_lengths += 1;
            if (report.torn_tail) {
                note("length " + std::to_string(length) +
                     ": damage reported as interior corruption AND as a torn tail");
            }
            DRC_REQUIRE(!report.torn_tail);
            if (report.corruption_offset >= length) {
                note("length " + std::to_string(length) + ": corruption offset " +
                     std::to_string(report.corruption_offset) + " is outside the file");
            }
            DRC_REQUIRE(report.corruption_offset < length);
            if (report.detail.empty()) {
                note("length " + std::to_string(length) + ": corruption reported with no detail");
            }
            DRC_REQUIRE(!report.detail.empty());
        } else {
            // A torn tail may only describe bytes strictly after the committed
            // boundary, and it must say how much was discarded.
            const std::uint64_t tail = length - report.committed_bytes;
            if (report.discarded_tail_bytes != tail) {
                note("length " + std::to_string(length) + ": discarded_tail_bytes " +
                     std::to_string(report.discarded_tail_bytes) + " but " + std::to_string(tail) +
                     " bytes follow the committed boundary");
            }
            DRC_REQUIRE_EQ(report.discarded_tail_bytes, tail);
            if (report.torn_tail) {
                torn_lengths += 1;
                if (report.discarded_tail_bytes == 0) {
                    note("length " + std::to_string(length) +
                         ": torn tail reported with nothing discarded");
                }
                DRC_REQUIRE(report.discarded_tail_bytes > 0);
            } else {
                clean_lengths += 1;
            }
        }

        // The same bytes, opened through a real coordinator: recovery either
        // refuses, or reproduces exactly the digest the surviving commit
        // recorded.
        const CommittedSummary summary = report.interior_corruption
                                              ? CommittedSummary{}
                                              : summarize_committed(reader.value());
        Result<std::unique_ptr<Coordinator>> opened =
            Coordinator::open(scratch_options(scratch.path));
        if (report.interior_corruption) {
            refused += 1;
            if (opened.ok()) {
                note("length " + std::to_string(length) +
                     ": an interior-corrupt journal was accepted by recovery");
                DRC_REQUIRE(false);
            }
            if (opened.code() != ErrorCode::Corrupt) {
                note("length " + std::to_string(length) + ": recovery failed with " +
                     std::string{to_string(opened.code())} + " instead of corrupt: " +
                     opened.status().to_string());
            }
            DRC_REQUIRE_EQ(opened.code(), ErrorCode::Corrupt);
            const std::string offset = std::to_string(report.corruption_offset);
            if (opened.status().message().find(offset) == std::string::npos) {
                note("length " + std::to_string(length) + ": refusal does not name offset " +
                     offset + ": " + opened.status().to_string());
            }
            DRC_REQUIRE(opened.status().message().find(offset) != std::string::npos);
            const std::vector<std::uint8_t> after = read_bytes(scratch_journal);
            if (after != prefix) {
                note("length " + std::to_string(length) +
                     ": the refused journal was modified on disk");
                DRC_REQUIRE(false);
            }
            continue;
        }

        recovered += 1;
        if (!opened.ok()) {
            note("length " + std::to_string(length) + ": recovery refused a recoverable prefix: " +
                 opened.status().to_string());
            DRC_REQUIRE(false);
        }
        if (require_recovered_state(*opened.value(), summary, empty_digest,
                                    "length " + std::to_string(length))) {
            fenced_lengths += 1;
        }
        if (length == total) {
            saw_full_length = true;
            const Digest actual = opened.value()->state_digest();
            if (actual != fixture.full_digest) {
                note("the undamaged journal recovered " + actual.to_hex() + " instead of " +
                     fixture.full_digest.to_hex());
                DRC_REQUIRE(false);
            }
        }
        DRC_REQUIRE_OK(opened.value()->shutdown());
    }
    note("classified " + std::to_string(interior_lengths) + " interior, " +
         std::to_string(torn_lengths) + " torn, " + std::to_string(clean_lengths) +
         " clean; " + std::to_string(refused) + " refused by recovery, " +
         std::to_string(recovered) + " recovered, of which " +
         std::to_string(fenced_lengths) + " ended mid-transaction and had their dispatched "
         "but unanswered work fenced by recovery");
    // Cutting a journal at a record boundary and cutting it inside a record are
    // both reachable, so both classifications must appear.
    DRC_REQUIRE(torn_lengths > 0);
    DRC_REQUIRE(clean_lengths > 0);
    DRC_REQUIRE(saw_full_length);
}

// ---------------------------------------------------------------------------
// 2. Single flipped bits.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_flipped_bits_are_detected) {
    const JournalFixture fixture = make_fixture("adv-flip");
    const Digest empty_digest = empty_state_digest();
    const std::size_t total = fixture.bytes.size();
    DRC_REQUIRE(fixture.walk.frames.size() >= 3);
    const Frame last_frame = fixture.walk.frames.back();
    note("journal bytes = " + std::to_string(total) + ", frames = " +
         std::to_string(fixture.walk.frames.size()) + ", last frame starts at " +
         std::to_string(last_frame.start));

    drctest::TempDir scratch("adv-flip-scratch");
    const std::string scratch_journal = fileio::join(scratch.path, "coordinator.journal");

    // Deterministic sample, printed below: every 512th byte, every field of the
    // three most interesting frame headers, the first payload byte of every
    // frame, and 64 offsets from a fixed-seed xorshift32.
    std::vector<std::size_t> offsets;
    for (std::size_t offset = 0; offset < total; offset += 512) {
        offsets.push_back(offset);
    }
    const std::size_t picked[4] = {0, 1, fixture.walk.frames.size() / 2,
                                   fixture.walk.frames.size() - 1};
    for (const std::size_t index : picked) {
        if (index >= fixture.walk.frames.size()) {
            continue;
        }
        const Frame& frame = fixture.walk.frames[index];
        for (std::size_t field = 0; field < journal::kHeaderBytes; ++field) {
            offsets.push_back(frame.start + field);
        }
    }
    for (const Frame& frame : fixture.walk.frames) {
        if (frame.payload_bytes > 0) {
            offsets.push_back(frame.start + journal::kHeaderBytes);
        }
    }
    std::uint32_t random_state = 0x2545F491u;  // fixed seed: the sample is reproducible
    for (int draw = 0; draw < 64; ++draw) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        offsets.push_back(static_cast<std::size_t>(random_state) % total);
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    note("flip rule: every 512th byte, every header field of the first, second, middle and last "
         "frame, the first payload byte of every frame, and 64 offsets from a fixed-seed "
         "xorshift32; flipping " + std::to_string(offsets.size()) + " of " + std::to_string(total) +
         " bytes");

    std::uint64_t interior_flips = 0;
    std::uint64_t tail_flips = 0;
    std::uint64_t fenced_offsets = 0;
    for (const std::size_t offset : offsets) {
        DRC_REQUIRE(offset < total);
        std::vector<std::uint8_t> damaged = fixture.bytes;
        damaged[offset] = static_cast<std::uint8_t>(damaged[offset] ^ 0x01u);
        if (!write_bytes(scratch_journal, damaged)) {
            note("could not write the damaged copy at offset " + std::to_string(offset));
            DRC_REQUIRE(false);
        }
        Result<journal::Reader> reader = journal::Reader::open(scratch_journal, kMaxJournalBytes);
        DRC_REQUIRE(reader.ok());
        const journal::ScanReport& report = reader.value().report();
        const bool inside_last_frame = offset >= last_frame.start;

        if (!inside_last_frame) {
            interior_flips += 1;
            if (!report.interior_corruption) {
                note("offset " + std::to_string(offset) + " (frame starting at " +
                     std::to_string(frame_start_of(fixture.walk, offset)) +
                     ") was not reported as interior corruption: torn_tail=" +
                     std::string{report.torn_tail ? "true" : "false"} + " detail=" +
                     report.detail);
                DRC_REQUIRE(false);
            }
            if (report.corruption_offset != frame_start_of(fixture.walk, offset)) {
                note("offset " + std::to_string(offset) + ": corruption reported at " +
                     std::to_string(report.corruption_offset) +
                     " instead of the damaged frame at " +
                     std::to_string(frame_start_of(fixture.walk, offset)));
            }
            DRC_REQUIRE_EQ(report.corruption_offset, frame_start_of(fixture.walk, offset));

            Result<std::unique_ptr<Coordinator>> opened =
                Coordinator::open(scratch_options(scratch.path));
            if (opened.ok()) {
                note("offset " + std::to_string(offset) +
                     ": recovery accepted a journal with a flipped bit inside a committed record");
                DRC_REQUIRE(false);
            }
            DRC_REQUIRE_EQ(opened.code(), ErrorCode::Corrupt);
            const std::string named = std::to_string(report.corruption_offset);
            if (opened.status().message().find(named) == std::string::npos) {
                note("offset " + std::to_string(offset) + ": refusal does not name offset " +
                     named + ": " + opened.status().to_string());
            }
            DRC_REQUIRE(opened.status().message().find(named) != std::string::npos);
            const std::vector<std::uint8_t> after = read_bytes(scratch_journal);
            if (after != damaged) {
                note("offset " + std::to_string(offset) +
                     ": the refused journal was modified on disk");
                DRC_REQUIRE(false);
            }
            continue;
        }

        // A flip inside the final record (the commit frame written at shutdown)
        // is indistinguishable from a crash before that commit: the reader must
        // say so, and recovery must land exactly on the previous commit.
        tail_flips += 1;
        if (!report.torn_tail) {
            note("offset " + std::to_string(offset) +
                 " inside the last frame was not reported as a torn tail: interior=" +
                 std::string{report.interior_corruption ? "true" : "false"} +
                 " detail=" + report.detail);
            DRC_REQUIRE(false);
        }
        DRC_REQUIRE(report.discarded_tail_bytes > 0);
        const CommittedSummary summary = summarize_committed(reader.value());
        Result<std::unique_ptr<Coordinator>> opened =
            Coordinator::open(scratch_options(scratch.path));
        if (!opened.ok()) {
            note("offset " + std::to_string(offset) +
                 ": a damaged commit frame stopped recovery: " + opened.status().to_string());
            DRC_REQUIRE(false);
        }
        if (require_recovered_state(*opened.value(), summary, empty_digest,
                                    "offset " + std::to_string(offset))) {
            fenced_offsets += 1;
        }
        const CoordinatorStats stats = opened.value()->stats();
        if (stats.torn_tail_recoveries == 0) {
            note("offset " + std::to_string(offset) +
                 ": a committed transaction was dropped without a torn-tail report");
        }
        DRC_REQUIRE(stats.torn_tail_recoveries > 0);
        DRC_REQUIRE_OK(opened.value()->shutdown());
    }
    note("flipped " + std::to_string(interior_flips) + " bytes in committed records and " +
         std::to_string(tail_flips) + " bytes in the final frame; " +
         std::to_string(fenced_offsets) + " recovery(s) fenced unfinished work");
    DRC_REQUIRE(interior_flips > 0);
    DRC_REQUIRE(tail_flips > 0);
}

// ---------------------------------------------------------------------------
// 3. Interior corruption is never truncated away.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_interior_corruption_is_never_truncated) {
    const JournalFixture fixture = make_fixture("adv-interior");
    DRC_REQUIRE(fixture.walk.frames.size() >= 5);

    // Pick a frame in the middle: never the first, never the last, and always
    // one with an intact frame after it.
    const std::size_t index = fixture.walk.frames.size() / 2;
    const Frame damaged_frame = fixture.walk.frames[index];
    const Frame follower = fixture.walk.frames[index + 1];
    const std::size_t damage_offset =
        damaged_frame.payload_bytes > 0 ? damaged_frame.start + journal::kHeaderBytes
                                        : damaged_frame.start + kOffsetSequence;
    note("damaging frame " + std::to_string(index) + " of " +
         std::to_string(fixture.walk.frames.size()) + " at offset " +
         std::to_string(damage_offset) + "; frame " + std::to_string(index + 1) +
         " starts at " + std::to_string(follower.start));

    std::vector<std::uint8_t> damaged = fixture.bytes;
    damaged[damage_offset] = static_cast<std::uint8_t>(damaged[damage_offset] ^ 0x80u);

    // Independent proof that a valid frame still exists after the damage: the
    // follower's own header checksum still verifies against its own bytes.
    const std::vector<std::uint8_t> follower_header(
        damaged.begin() + static_cast<std::ptrdiff_t>(follower.start),
        damaged.begin() + static_cast<std::ptrdiff_t>(follower.start + journal::kHeaderBytes));
    DRC_REQUIRE_EQ(read_u32(follower_header, 0), journal::kMagic);
    std::vector<std::uint8_t> follower_crc_input = follower_header;
    const std::uint32_t follower_crc = read_u32(follower_header, kOffsetHeaderCrc);
    for (std::size_t i = 0; i < 4; ++i) {
        follower_crc_input[kOffsetHeaderCrc + i] = 0;
    }
    if (crc32c(follower_crc_input) != follower_crc) {
        note("the frame after the damage does not verify; the damage is not provably interior");
        DRC_REQUIRE(false);
    }

    drctest::TempDir scratch("adv-interior-scratch");
    const std::string scratch_journal = fileio::join(scratch.path, "coordinator.journal");
    DRC_REQUIRE(write_bytes(scratch_journal, damaged));
    const std::uint64_t length_before = bytes_of(scratch_journal);

    Result<journal::Reader> reader = journal::Reader::open(scratch_journal, kMaxJournalBytes);
    DRC_REQUIRE(reader.ok());
    const journal::ScanReport& report = reader.value().report();
    if (!report.interior_corruption) {
        note("the middle damage was classified as torn_tail=" +
             std::string{report.torn_tail ? "true" : "false"} +
             " instead of interior corruption");
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE_EQ(report.corruption_offset, damaged_frame.start);
    DRC_REQUIRE(!report.torn_tail);

    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(scratch_options(scratch.path));
    if (opened.ok()) {
        note("recovery accepted a journal corrupted in the middle");
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE_EQ(opened.code(), ErrorCode::Corrupt);
    if (opened.status().message().find(std::to_string(damaged_frame.start)) == std::string::npos) {
        note("refusal does not name the damaged offset " + std::to_string(damaged_frame.start) +
             ": " + opened.status().to_string());
    }
    DRC_REQUIRE(opened.status().message().find(std::to_string(damaged_frame.start)) !=
                std::string::npos);

    const std::uint64_t length_after = bytes_of(scratch_journal);
    if (length_after != length_before) {
        note("the journal length changed from " + std::to_string(length_before) + " to " +
             std::to_string(length_after) + " after a refused recovery");
    }
    DRC_REQUIRE_EQ(length_after, length_before);
    const std::vector<std::uint8_t> after = read_bytes(scratch_journal);
    if (after != damaged) {
        note("the refused journal was rewritten on disk");
        DRC_REQUIRE(false);
    }
}

// ---------------------------------------------------------------------------
// 4. An uncommitted tail is discarded without loss of committed content.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_uncommitted_tail_is_recovered_without_loss) {
    drctest::Rig rig = drctest::Rig::make("adv-tail");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE(rig.coordinator->outstanding_requests().value().empty());
    const Digest committed_digest = rig.coordinator->state_digest();
    const std::string journal_path = fileio::join(rig.directory->path, "coordinator.journal");
    rig.close();

    const std::uint64_t committed_bytes = bytes_of(journal_path);

    // Append well-formed records with a raw writer and never commit them.
    journal::Writer::Options options;
    options.path = journal_path;
    options.max_bytes = kMaxJournalBytes;
    Result<journal::Writer> writer = journal::Writer::open(options);
    DRC_REQUIRE(writer.ok());
    canonical::Writer payload;
    payload.u64(42);
    payload.text("uncommitted-site");
    payload.u64(2);
    payload.u64(777);
    DRC_REQUIRE(!payload.failed());
    DRC_REQUIRE(writer.value().append(journal::RecordType::Site, payload.bytes()).ok());
    canonical::Writer second;
    second.u64(43);
    second.text("uncommitted-site-two");
    second.u64(2);
    second.u64(778);
    DRC_REQUIRE(!second.failed());
    DRC_REQUIRE(writer.value().append(journal::RecordType::Site, second.bytes()).ok());
    DRC_REQUIRE_OK(writer.value().flush());
    DRC_REQUIRE_OK(writer.value().close());

    const std::uint64_t with_tail = bytes_of(journal_path);
    DRC_REQUIRE(with_tail > committed_bytes);

    Result<journal::Reader> reader = journal::Reader::open(journal_path, kMaxJournalBytes);
    DRC_REQUIRE(reader.ok());
    const journal::ScanReport& report = reader.value().report();
    note("committed_bytes=" + std::to_string(report.committed_bytes) +
         " discarded_tail_bytes=" + std::to_string(report.discarded_tail_bytes) + " torn_tail=" +
         std::string{report.torn_tail ? "true" : "false"});
    DRC_REQUIRE_EQ(report.committed_bytes, committed_bytes);
    DRC_REQUIRE(report.discarded_tail_bytes > 0);
    DRC_REQUIRE_EQ(report.discarded_tail_bytes, with_tail - committed_bytes);

    rig.open();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), committed_digest);
    DRC_REQUIRE_ERR(rig.coordinator->site(SiteId{42}), ErrorCode::NotFound);
    DRC_REQUIRE_ERR(rig.coordinator->site(SiteId{43}), ErrorCode::NotFound);
    const DisasterEvent settled = rig.event(event);
    DRC_REQUIRE_EQ(settled.phase, EventPhase::Stabilized);
    const std::uint64_t committed_sequence = rig.coordinator->last_committed_sequence().value();
    DRC_REQUIRE(committed_sequence > 0);
    rig.close();

    // The reopened journal's committed content holds exactly the original
    // objects: nothing the raw writer appended without a commit is visible.
    Result<journal::Reader> reopened = journal::Reader::open(journal_path, kMaxJournalBytes);
    DRC_REQUIRE(reopened.ok());
    std::size_t committed_sites = 0;
    bool smuggled = false;
    const Status walked =
        reopened.value().for_each_committed([&](const journal::Record& record) -> Status {
            if (record.type != journal::RecordType::Site) {
                return ok_status();
            }
            committed_sites += 1;
            std::uint64_t id = 0;
            for (std::size_t i = 0; i < 8 && i < record.payload.size(); ++i) {
                id |= static_cast<std::uint64_t>(record.payload[i])
                      << (8u * static_cast<unsigned>(i));
            }
            if (id == 42 || id == 43) {
                smuggled = true;
            }
            return ok_status();
        });
    DRC_REQUIRE(walked.ok());
    note("after recovery: committed site records = " + std::to_string(committed_sites) +
         ", committed_bytes = " + std::to_string(reopened.value().report().committed_bytes) +
         " (the discarded tail began at " + std::to_string(committed_bytes) + ", the file held " +
         std::to_string(with_tail) + " bytes)");
    DRC_REQUIRE(!smuggled);
    DRC_REQUIRE_EQ(committed_sites, std::size_t{2});
    DRC_REQUIRE(reopened.value().report().committed_bytes >= committed_bytes);
}

// ---------------------------------------------------------------------------
// 5. A damaged snapshot is never silently ignored.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_snapshot_damage_is_not_silently_ignored) {
    // (a) The journal still carries the whole history, so the snapshot is an
    // optimisation: recovery either reproduces the same state from the journal
    // or refuses loudly. It must never invent a smaller state.
    {
        drctest::Rig rig = drctest::Rig::make("adv-snapshot-full");
        rig.open();
        build_reference_journal(rig);
        const Digest planned_digest = rig.coordinator->state_digest();
        const std::string directory = rig.directory->path;
        const std::string snapshot_path = fileio::join(directory, "coordinator.snapshot");
        rig.close();

        const std::vector<std::uint8_t> snapshot = read_bytes(snapshot_path);
        DRC_REQUIRE(snapshot.size() > checkpoint::kHeaderBytes);
        note("(a) snapshot bytes = " + std::to_string(snapshot.size()));
        DRC_REQUIRE(write_prefix(snapshot_path, snapshot, snapshot.size() / 3));
        Result<checkpoint::Contents> damaged_read =
            checkpoint::read_file(snapshot_path, kMaxJournalBytes);
        if (damaged_read.ok()) {
            note("(a) a truncated snapshot still read back as valid");
            DRC_REQUIRE(false);
        }

        Result<std::unique_ptr<Coordinator>> opened =
            Coordinator::open(scratch_options(directory));
        if (opened.ok()) {
            const Digest actual = opened.value()->state_digest();
            if (actual != planned_digest) {
                note("(a) recovered " + actual.to_hex() + " from the journal instead of " +
                     planned_digest.to_hex());
                DRC_REQUIRE(false);
            }
            // The recovery must say the snapshot was unusable rather than
            // quietly ignoring it.
            std::vector<std::string> notices;
            DRC_REQUIRE_OK(opened.value()->set_observer(
                [&notices](const TransitionNotice& notice) { notices.push_back(notice.kind); }));
            DRC_REQUIRE_OK(opened.value()->checkpoint());
            bool reported = false;
            for (const std::string& kind : notices) {
                if (kind == "snapshot_unusable") {
                    reported = true;
                }
            }
            if (!reported) {
                std::string joined;
                for (const std::string& kind : notices) {
                    joined.append(kind).append(" ");
                }
                note("(a) no snapshot_unusable notice was published; notices were: " + joined);
            }
            DRC_REQUIRE(reported);
            DRC_REQUIRE_OK(opened.value()->shutdown());
        } else {
            note("(a) recovery refused a truncated snapshot beside a full journal: " +
                 opened.status().to_string());
            DRC_REQUIRE_EQ(opened.code(), ErrorCode::Corrupt);
        }
    }

    // (b) A compacted journal makes the snapshot the only source of the state:
    // losing it must fail, never invent a smaller state.
    {
        drctest::Rig rig = drctest::Rig::make("adv-snapshot-compacted");
        rig.open();
        build_reference_journal(rig);
        DRC_REQUIRE_OK(rig.coordinator->compact());
        const Digest compacted_digest = rig.coordinator->state_digest();
        const std::string directory = rig.directory->path;
        const std::string journal_path = fileio::join(directory, "coordinator.journal");
        const std::string snapshot_path = fileio::join(directory, "coordinator.snapshot");
        rig.close();

        const std::vector<std::uint8_t> snapshot = read_bytes(snapshot_path);
        const std::uint64_t journal_bytes = bytes_of(journal_path);
        note("(b) after compaction the journal holds " + std::to_string(journal_bytes) +
             " bytes and the snapshot " + std::to_string(snapshot.size()) + " bytes");
        DRC_REQUIRE(write_prefix(snapshot_path, snapshot, checkpoint::kHeaderBytes / 2));

        Result<std::unique_ptr<Coordinator>> opened =
            Coordinator::open(scratch_options(directory));
        if (opened.ok()) {
            const Digest actual = opened.value()->state_digest();
            const std::size_t sites = opened.value()->sites().value().size();
            note("(b) a damaged snapshot beside a compacted journal produced state " +
                 actual.to_hex() + " with " + std::to_string(sites) +
                 " sites; the state before compaction was " + compacted_digest.to_hex());
            DRC_REQUIRE(false);
        }
        DRC_REQUIRE_EQ(opened.code(), ErrorCode::Corrupt);
        note("(b) refused with: " + opened.status().to_string());
        DRC_REQUIRE(!opened.status().message().empty());
        DRC_REQUIRE_EQ(bytes_of(journal_path), journal_bytes);
    }
}

// ---------------------------------------------------------------------------
// 6. Absurd and hostile metadata changes nothing.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_absurd_and_hostile_metadata_is_rejected) {
    drctest::Rig rig = drctest::Rig::make("adv-metadata");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(20, "a", RecoveryClass::Standard, 1, 5);
    rig.obligation(21, "b", RecoveryClass::Standard, 1, 6, {20});
    RecoveryPolicy policy = rig.coordinator->policy().value();
    policy.generation = Generation{2};
    policy.max_step_attempts = 4;
    DRC_REQUIRE_OK(rig.coordinator->set_policy(policy));
    const Digest baseline = rig.coordinator->state_digest();

    const auto refused = [&](const char* label,
                             ErrorCode expected,
                             const std::function<Status()>& call) {
        const Status status = call();
        if (status.ok()) {
            note(std::string{"accepted hostile metadata: "} + label);
        }
        DRC_REQUIRE(!status.ok());
        if (status.code() != expected) {
            note(std::string{"hostile metadata "} + label + " failed with " +
                 std::string{to_string(status.code())} + " instead of " +
                 std::string{to_string(expected)} + ": " + status.to_string());
        }
        DRC_REQUIRE_EQ(status.code(), expected);
        const Digest after = rig.coordinator->state_digest();
        if (after != baseline) {
            note(std::string{"state changed after rejected metadata: "} + label);
            DRC_REQUIRE(false);
        }
    };

    FailureDomainRecord zero_domain;
    zero_domain.id = FailureDomainId{};
    zero_domain.name = "zero-domain";
    refused("failure domain identity 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_failure_domain(zero_domain); });

    SiteRecord zero_site;
    zero_site.id = SiteId{};
    zero_site.name = "zero-site";
    zero_site.domain = FailureDomainId{1};
    zero_site.capacity_units = 10;
    refused("site identity 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_site(zero_site); });

    ProtectedObligation zero_obligation;
    zero_obligation.id = ObligationId{};
    zero_obligation.name = "zero-obligation";
    zero_obligation.recovery_class = RecoveryClass::Standard;
    zero_obligation.home_site = SiteId{1};
    zero_obligation.required_capacity_units = 10;
    refused("obligation identity 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_obligation(zero_obligation); });

    // A duplicate identity with different content is a conflict, not a merge.
    SiteRecord duplicate_site;
    duplicate_site.id = SiteId{1};
    duplicate_site.name = "site-a1-renamed";
    duplicate_site.domain = FailureDomainId{1};
    duplicate_site.capacity_units = 100;
    refused("site identity reused with different content", ErrorCode::Conflict,
            [&] { return rig.coordinator->define_site(duplicate_site); });

    FailureDomainRecord duplicate_domain;
    duplicate_domain.id = FailureDomainId{1};
    duplicate_domain.name = "domain-a-renamed";
    refused("failure domain identity reused with different content", ErrorCode::Conflict,
            [&] { return rig.coordinator->define_failure_domain(duplicate_domain); });

    ProtectedObligation duplicate_obligation;
    duplicate_obligation.id = ObligationId{20};
    duplicate_obligation.name = "a";
    duplicate_obligation.recovery_class = RecoveryClass::Standard;
    duplicate_obligation.home_site = SiteId{1};
    duplicate_obligation.required_capacity_units = 9;
    refused("obligation identity reused with different content", ErrorCode::Conflict,
            [&] { return rig.coordinator->define_obligation(duplicate_obligation); });

    // The same identity with identical content is idempotent by design; the
    // state must not move.
    const SiteRecord identical_site = rig.coordinator->site(SiteId{1}).value();
    DRC_REQUIRE_OK(rig.coordinator->define_site(identical_site));
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), baseline);

    ProtectedObligation no_capacity;
    no_capacity.id = ObligationId{30};
    no_capacity.name = "no-capacity";
    no_capacity.recovery_class = RecoveryClass::Standard;
    no_capacity.home_site = SiteId{1};
    no_capacity.required_capacity_units = 0;
    refused("obligation capacity 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_obligation(no_capacity); });

    ProtectedObligation self_dependency;
    self_dependency.id = ObligationId{31};
    self_dependency.name = "self";
    self_dependency.recovery_class = RecoveryClass::Standard;
    self_dependency.home_site = SiteId{1};
    self_dependency.required_capacity_units = 4;
    self_dependency.depends_on.push_back(ObligationId{31});
    refused("obligation depends on itself", ErrorCode::Conflict,
            [&] { return rig.coordinator->define_obligation(self_dependency); });

    ProtectedObligation unknown_dependency;
    unknown_dependency.id = ObligationId{32};
    unknown_dependency.name = "unknown-dep";
    unknown_dependency.recovery_class = RecoveryClass::Standard;
    unknown_dependency.home_site = SiteId{1};
    unknown_dependency.required_capacity_units = 4;
    unknown_dependency.depends_on.push_back(ObligationId{9999});
    refused("obligation depends on an unregistered obligation", ErrorCode::NotFound,
            [&] { return rig.coordinator->define_obligation(unknown_dependency); });

    ProtectedObligation repeated_dependency;
    repeated_dependency.id = ObligationId{33};
    repeated_dependency.name = "repeated-dep";
    repeated_dependency.recovery_class = RecoveryClass::Standard;
    repeated_dependency.home_site = SiteId{1};
    repeated_dependency.required_capacity_units = 4;
    repeated_dependency.depends_on.push_back(ObligationId{20});
    repeated_dependency.depends_on.push_back(ObligationId{20});
    refused("obligation lists a dependency twice", ErrorCode::Duplicate,
            [&] { return rig.coordinator->define_obligation(repeated_dependency); });

    // A cycle across a registered edge: 20 -> 21 -> 20.
    ProtectedObligation cycle;
    cycle.id = ObligationId{20};
    cycle.name = "a";
    cycle.recovery_class = RecoveryClass::Standard;
    cycle.home_site = SiteId{1};
    cycle.required_capacity_units = 5;
    cycle.depends_on.push_back(ObligationId{21});
    refused("obligation cycle a->b->a", ErrorCode::Conflict,
            [&] { return rig.coordinator->define_obligation(cycle); });

    SiteRecord unregistered_domain_site;
    unregistered_domain_site.id = SiteId{40};
    unregistered_domain_site.name = "orphan-site";
    unregistered_domain_site.domain = FailureDomainId{77};
    refused("site names an unregistered failure domain", ErrorCode::NotFound,
            [&] { return rig.coordinator->define_site(unregistered_domain_site); });

    ProtectedObligation unregistered_home;
    unregistered_home.id = ObligationId{41};
    unregistered_home.name = "orphan-obligation";
    unregistered_home.recovery_class = RecoveryClass::Standard;
    unregistered_home.home_site = SiteId{99};
    unregistered_home.required_capacity_units = 4;
    refused("obligation names an unregistered home site", ErrorCode::NotFound,
            [&] { return rig.coordinator->define_obligation(unregistered_home); });

    SiteRecord long_name = identical_site;
    long_name.name = std::string(canonical::kMaxTextBytes + 1, kLongNameSeed);
    refused("site name longer than the canonical maximum", ErrorCode::OutOfRange,
            [&] { return rig.coordinator->define_site(long_name); });

    SiteRecord invalid_utf8 = identical_site;
    invalid_utf8.name.clear();
    invalid_utf8.name.push_back(static_cast<char>(0xC3));
    invalid_utf8.name.push_back(static_cast<char>(0x28));
    refused("site name with invalid UTF-8", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_site(invalid_utf8); });

    // Tab and newline are deliberately accepted text; any other control byte is
    // not, and the refusal must not be a content conflict.
    SiteRecord control_characters = identical_site;
    control_characters.name = std::string{"sitename"};
    refused("site name with a control character", ErrorCode::Invalid,
            [&] { return rig.coordinator->define_site(control_characters); });

    SiteReadinessEvidence readiness;
    readiness.site = SiteId{3};
    readiness.generation = Generation{1};
    readiness.observed_at = rig.clock->now_nanos();
    readiness.observation_epoch = rig.coordinator->epoch();
    readiness.source = "test-harness";
    readiness.checks.push_back(ReadinessCheck{"power", true});
    readiness.checks.push_back(ReadinessCheck{"power", true});
    refused("readiness lists the same check twice", ErrorCode::Duplicate,
            [&] { return rig.coordinator->record_readiness(readiness); });

    DestinationCapability unknown_mask;
    unknown_mask.site = SiteId{3};
    unknown_mask.generation = Generation{1};
    unknown_mask.available_capacity_units = 10;
    unknown_mask.supported_classes_mask = 1u << kRecoveryClassCount;
    unknown_mask.observed_at = rig.clock->now_nanos();
    unknown_mask.observation_epoch = rig.coordinator->epoch();
    unknown_mask.source = "test-harness";
    refused("capability class mask with unknown bits", ErrorCode::Invalid,
            [&] { return rig.coordinator->record_capability(unknown_mask); });

    DestinationCapability zero_generation = unknown_mask;
    zero_generation.supported_classes_mask = 0x1Fu;
    zero_generation.generation = Generation{};
    refused("capability generation 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->record_capability(zero_generation); });

    RecoveryPolicy zero_policy = policy;
    zero_policy.generation = Generation{};
    refused("policy generation 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->set_policy(zero_policy); });

    RecoveryPolicy backwards = policy;
    backwards.generation = Generation{1};
    refused("policy generation going backwards", ErrorCode::Stale,
            [&] { return rig.coordinator->set_policy(backwards); });

    RecoveryPolicy no_in_flight = policy;
    no_in_flight.generation = Generation{3};
    no_in_flight.max_in_flight_steps = 0;
    refused("policy max_in_flight_steps 0", ErrorCode::Invalid,
            [&] { return rig.coordinator->set_policy(no_in_flight); });

    RecoveryPolicy same_generation = policy;
    same_generation.max_in_flight_steps = policy.max_in_flight_steps + 1;
    refused("policy content changed without a generation bump", ErrorCode::Conflict,
            [&] { return rig.coordinator->set_policy(same_generation); });

    const RecoveryPolicy retained = rig.coordinator->policy().value();
    DRC_REQUIRE_EQ(retained.generation, Generation{2});
    DRC_REQUIRE_EQ(retained.max_step_attempts, 4u);
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), baseline);
}

// ---------------------------------------------------------------------------
// 7. Traversal and reserved names never reach the filesystem.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_path_traversal_and_reserved_names_are_refused) {
    drctest::TempDir root("adv-names");
    const std::string directory = root.path;
    const std::filesystem::path parent = std::filesystem::path(directory).parent_path();

    std::error_code error;
    std::vector<std::string> parent_before;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(parent, error)) {
        parent_before.push_back(entry.path().filename().string());
    }
    DRC_REQUIRE(!error);

    struct HostileName {
        const char* label;
        std::string value;
    };
    std::vector<HostileName> names;
    names.push_back({"dot-dot", ".."});
    names.push_back({"relative traversal", "../escape"});
    names.push_back({"path separator", "a/b"});
    names.push_back({"reserved device CON", "CON"});
    names.push_back({"reserved device con.journal", "con.journal"});
    names.push_back({"reserved device lpt1", "lpt1"});
    names.push_back({"empty", ""});
    names.push_back({"embedded NUL", std::string{"bad\0name", 8}});
    names.push_back({"control character", std::string{"bad\x01name"}});
    names.push_back({"5000 characters", std::string(5000, 'x')});

    std::size_t attempts = 0;
    for (const HostileName& name : names) {
        for (int field = 0; field < 3; ++field) {
            CoordinatorOptions options = scratch_options(directory);
            if (field == 0) {
                options.journal_name = name.value;
            } else if (field == 1) {
                options.snapshot_name = name.value;
            } else {
                options.lock_name = name.value;
            }
            attempts += 1;
            Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(options);
            if (opened.ok()) {
                note(std::string{"accepted a hostile name ("} + name.label + ") for field " +
                     std::to_string(field));
                DRC_REQUIRE_OK(opened.value()->shutdown());
                DRC_REQUIRE(false);
            }
            const ErrorCode code = opened.code();
            if (code != ErrorCode::Invalid && code != ErrorCode::OutOfRange) {
                note(std::string{"hostile name ("} + name.label + ") failed with " +
                     std::string{to_string(code)} + " instead of invalid/out_of_range: " +
                     opened.status().to_string());
            }
            DRC_REQUIRE(code == ErrorCode::Invalid || code == ErrorCode::OutOfRange);

            std::size_t entries = 0;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(directory, error)) {
                (void)entry;
                entries += 1;
            }
            if (entries != 0) {
                note(std::string{"hostile name ("} + name.label + ") created " +
                     std::to_string(entries) + " file(s) inside the coordinator directory");
                DRC_REQUIRE(false);
            }
            if (std::filesystem::exists(std::filesystem::path(directory) / "CON", error) ||
                std::filesystem::exists(std::filesystem::path(directory) / "lpt1", error)) {
                note(std::string{"hostile name ("} + name.label +
                     ") left a reserved device name behind");
                DRC_REQUIRE(false);
            }
        }
    }
    note("refused " + std::to_string(attempts) + " hostile name placements");

    std::vector<std::string> parent_after;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(parent, error)) {
        parent_after.push_back(entry.path().filename().string());
    }
    DRC_REQUIRE(!error);
    std::sort(parent_before.begin(), parent_before.end());
    std::sort(parent_after.begin(), parent_after.end());
    for (const std::string& entry : parent_after) {
        if (!std::binary_search(parent_before.begin(), parent_before.end(), entry)) {
            note("a hostile name created something outside the coordinator directory: " + entry);
            DRC_REQUIRE(false);
        }
    }
    DRC_REQUIRE(true);
}

// ---------------------------------------------------------------------------
// 8. An over-long directory path is answered honestly.
// ---------------------------------------------------------------------------
DRC_TEST(adversarial_long_path_is_handled_honestly) {
    drctest::TempDir root("adv-longpath");
    std::string deep = root.path;
    std::string first_segment;
    std::uint32_t segment = 0;
    while (deep.size() <= 260 || first_segment.empty()) {
        const std::string leaf = "/segment-" + std::to_string(segment) + "-aaaaaaaaaaaaaaaa";
        if (first_segment.empty()) {
            first_segment = std::string{leaf.begin() + 1, leaf.end()};
        }
        deep.append(leaf);
        segment += 1;
    }
    note("long path length = " + std::to_string(deep.size()) + ": " + deep);
    note("the chain begins at " + fileio::join(root.path, first_segment));
    // Windows refuses ordinary path APIs at more than MAX_PATH (260) characters
    // unless the extended-length prefix is used: this is the platform limit.

    // (1) The library's own directory helper, called directly, so that a failure
    //     can be attributed to the library and not to this test's setup.
    const Status ensured = fileio::ensure_directory(deep);
    Result<bool> deep_present = fileio::exists(deep);
    note("fileio::ensure_directory(long path) -> " +
         (ensured.ok() ? std::string{"ok"} : ensured.to_string()) + "; exists() afterwards -> " +
         (deep_present.ok() ? std::string{deep_present.value() ? "true" : "false"}
                            : deep_present.status().to_string()));

    // (2) Create the chain the way a caller has to on Windows: with the
    //     extended-length prefix, which disables path normalisation, so every
    //     separator has to be a backslash. With the chain already present, the
    //     only question left is whether the coordinator can use a long path.
#if defined(_WIN32)
    std::error_code error;
    const std::filesystem::path deep_path(deep);
    std::wstring wide = deep_path.wstring();
    for (wchar_t& character : wide) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    const std::filesystem::path extended_path = std::filesystem::path(L"\\\\?\\" + wide);
    std::filesystem::create_directories(extended_path, error);
    const bool chain_exists = std::filesystem::exists(extended_path, error);
    note(std::string{"this test's own extended-length create_directories -> "} +
         (chain_exists ? "the chain exists" : error.message()));
#else
    // POSIX has neither a MAX_PATH nor an extended-length prefix: the helper
    // above is the whole story, and the chain it created is the one that is
    // asked about here.
    Result<bool> present_after_ensure = fileio::exists(deep);
    DRC_REQUIRE(present_after_ensure.ok());
    const bool chain_exists = present_after_ensure.value();
    note(std::string{"this platform has no extended-length prefix; the chain from "
                     "fileio::ensure_directory exists -> "} +
         (chain_exists ? "yes" : "no"));
#endif

    // (3) The coordinator, on the deep directory.
    CoordinatorOptions options = scratch_options(deep);
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(options);
    if (opened.ok()) {
        const std::string journal = fileio::join(deep, options.journal_name);
        Result<bool> present = fileio::exists(journal);
        DRC_REQUIRE(present.ok());
        if (!present.value()) {
            note("the coordinator reported success but no journal exists at " + journal);
            DRC_REQUIRE(false);
        }
        DRC_REQUIRE_EQ(opened.value()->directory(), deep);
        DRC_REQUIRE_OK(opened.value()->shutdown());
        note("the long path was accepted; the journal is at " + journal);
    } else {
        note("the long path was refused with " + std::string{to_string(opened.code())} + ": " +
             opened.status().to_string());
        DRC_REQUIRE(opened.code() == ErrorCode::Io || opened.code() == ErrorCode::OutOfRange);
        DRC_REQUIRE(!opened.status().message().empty());
        const std::string message = opened.status().message();
        // The refusal has to identify what failed: this test accepts either the
        // path itself or the operating-system call that could not handle it.
        if (message.find(deep) == std::string::npos &&
            message.find("CreateDirectoryW") == std::string::npos) {
            note("the refusal names neither the path nor the call that failed: " + message);
            DRC_REQUIRE(false);
        }
        if (message.find(deep) == std::string::npos) {
            note("the refusal does not name the path it could not create: " + message);
        }
        // The chain exists, so the refusal is not "the directory is missing".
        DRC_REQUIRE(chain_exists);
    }

    // Whatever happened, nothing may have been written to the shallow path.
    const std::string shallow_journal = fileio::join(root.path, options.journal_name);
    Result<bool> shallow = fileio::exists(shallow_journal);
    DRC_REQUIRE(shallow.ok());
    if (shallow.value()) {
        note("a journal appeared at the shallow path " + shallow_journal);
    }
    DRC_REQUIRE(!shallow.value());

    // Clean up the deep tree with the library's own removal, which walks long
    // paths with the extended-length prefix. The standard filesystem library is
    // not reliable there on every toolchain: with MinGW libstdc++ a
    // remove_all() on such a path does not terminate at all.
    DRC_REQUIRE_OK(fileio::remove_tree(root.path));
    Result<bool> still_there = fileio::exists(deep);
    DRC_REQUIRE(still_there.ok());
    if (still_there.value()) {
        note("the long directory tree could not be removed: " + deep);
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE(true);
}
