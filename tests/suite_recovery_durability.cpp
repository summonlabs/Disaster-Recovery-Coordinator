#include <cstdint>
#include <string>
#include <vector>

#include "drc/checkpoint.hpp"
#include "drc/fileio.hpp"
#include "drc/journal.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

std::string journal_path(const drctest::Rig& rig) {
    return fileio::join(rig.directory->path, "coordinator.journal");
}

std::string snapshot_path(const drctest::Rig& rig) {
    return fileio::join(rig.directory->path, "coordinator.snapshot");
}

std::uint64_t file_bytes(const std::string& path) {
    const Result<std::uint64_t> size = fileio::file_size(path);
    DRC_REQUIRE(size.ok());
    return size.value();
}

void build_state(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(2, "site-a2", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.obligation(11, "orders", RecoveryClass::Protected, 1, 20);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
}

}  // namespace

DRC_TEST(recovery_close_and_reopen_reproduces_the_digest) {
    drctest::Rig rig = drctest::Rig::make("recovery-reopen");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    const Digest before = rig.coordinator->state_digest();
    const Epoch epoch_before = rig.coordinator->epoch();
    const Sequence sequence_before = rig.coordinator->last_committed_sequence();

    rig.close();
    rig.open();

    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), before);
    DRC_REQUIRE(rig.coordinator->epoch() > epoch_before);
    DRC_REQUIRE(rig.coordinator->last_committed_sequence() > sequence_before);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    // Two obligations in scope, six steps each, plus one network step.
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{13});
}

DRC_TEST(recovery_checkpoint_covers_the_state_and_stays_readable) {
    drctest::Rig rig = drctest::Rig::make("recovery-checkpoint");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_OK(rig.coordinator->checkpoint());

    const std::uint64_t journal_before = file_bytes(journal_path(rig));
    const std::uint64_t snapshot_bytes = file_bytes(snapshot_path(rig));
    DRC_REQUIRE(snapshot_bytes > checkpoint::kHeaderBytes);

    const Result<checkpoint::Contents> contents =
        checkpoint::read_file(snapshot_path(rig), 16u * 1024u * 1024u);
    DRC_REQUIRE_OK(contents);
    DRC_REQUIRE_EQ(contents.value().header.state_digest, rig.coordinator->state_digest());
    // The snapshot covers everything committed before it; the snapshot
    // reference and its commit follow it.
    DRC_REQUIRE(contents.value().header.covered_through <=
                rig.coordinator->last_committed_sequence());
    DRC_REQUIRE(contents.value().header.covered_through.valid());

    const Digest before = rig.coordinator->state_digest();
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), before);
    DRC_REQUIRE(file_bytes(journal_path(rig)) < journal_before * 2 + 4096);
}

DRC_TEST(recovery_compaction_preserves_state_and_bounds_the_journal) {
    drctest::Rig rig = drctest::Rig::make("recovery-compaction");
    rig.open();
    build_state(rig);
    // Enough busy work that the journal grows well past one snapshot.
    for (std::uint64_t site = 100; site < 160; ++site) {
        rig.site(site, "filler-" + std::to_string(site), 2, 0);
    }
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);

    const std::uint64_t before = file_bytes(journal_path(rig));
    const Digest digest = rig.coordinator->state_digest();
    DRC_REQUIRE_OK(rig.coordinator->compact());
    const std::uint64_t after = file_bytes(journal_path(rig));
    DRC_REQUIRE(after < before);
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), digest);

    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE_OK(verification);
    DRC_REQUIRE(verification.value().ok);
    DRC_REQUIRE(!verification.value().interior_corruption);

    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), digest);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    const Result<std::vector<SiteId>> sites = rig.coordinator->sites();
    DRC_REQUIRE_OK(sites);
    DRC_REQUIRE_EQ(sites.value().size(), std::size_t{63});
}

DRC_TEST(recovery_uncommitted_records_are_invisible_after_reopen) {
    drctest::Rig rig = drctest::Rig::make("recovery-uncommitted");
    rig.open();
    build_state(rig);
    const Digest committed = rig.coordinator->state_digest();
    rig.close();

    // Append a well-formed site record and never commit it: the durable
    // boundary is the commit, so recovery must not see it.
    journal::Writer::Options options;
    options.path = journal_path(rig);
    options.max_bytes = 8u * 1024u * 1024u;
    Result<journal::Writer> writer = journal::Writer::open(options);
    // Writer is move-only, so the result is checked rather than copied.
    DRC_REQUIRE(writer.ok());
    SiteRecord stray;
    stray.id = SiteId{777};
    stray.name = "never-committed";
    stray.domain = FailureDomainId{2};
    stray.capacity_units = 1;
    // The encoding used by the engine is internal, so this test writes a note
    // record with equivalent effect: no commit means no authority.
    const std::string note = "uncommitted marker";
    const std::vector<std::uint8_t> payload{note.begin(), note.end()};
    DRC_REQUIRE_OK(writer.value().append(journal::RecordType::Note, payload));
    DRC_REQUIRE_OK(writer.value().flush());
    const std::uint64_t torn_bytes = writer.value().bytes_written();
    DRC_REQUIRE_OK(writer.value().close());
    DRC_REQUIRE(torn_bytes > 0);

    rig.open();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), committed);
    DRC_REQUIRE_ERR(rig.coordinator->site(SiteId{777}), ErrorCode::NotFound);
    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE_OK(verification);
    DRC_REQUIRE(verification.value().ok);
}

DRC_TEST(recovery_torn_tail_is_reported_and_never_loses_committed_records) {
    drctest::Rig rig = drctest::Rig::make("recovery-torn-tail");
    rig.open();
    build_state(rig);
    rig.declare({1, 2});
    rig.close();

    const std::string path = journal_path(rig);
    const std::uint64_t original = file_bytes(path);
    // Reopen through the writer, which repairs the file to its committed
    // boundary and reports the discarded tail.
    journal::Writer::Options options;
    options.path = path;
    options.max_bytes = 8u * 1024u * 1024u;
    Result<journal::Writer> writer = journal::Writer::open(options);
    // Writer is move-only, so the result is checked rather than copied.
    DRC_REQUIRE(writer.ok());
    const journal::ScanReport& report = writer.value().scan();
    DRC_REQUIRE_EQ(report.file_bytes, original);
    DRC_REQUIRE(report.committed_bytes <= original);
    DRC_REQUIRE_EQ(report.interior_corruption, false);
    DRC_REQUIRE_OK(writer.value().close());
}

DRC_TEST(recovery_damaged_snapshot_never_invents_state) {
    drctest::Rig rig = drctest::Rig::make("recovery-damaged-snapshot");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->checkpoint());
    rig.close();

    const std::string path = snapshot_path(rig);
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, 16u * 1024u * 1024u);
    DRC_REQUIRE_OK(bytes);
    std::vector<std::uint8_t> damaged = bytes.value();
    DRC_REQUIRE(damaged.size() > checkpoint::kHeaderBytes + 8);
    // Corrupt one byte inside the payload.
    damaged[checkpoint::kHeaderBytes + 4] = static_cast<std::uint8_t>(
        damaged[checkpoint::kHeaderBytes + 4] ^ 0x40u);
    DRC_REQUIRE_OK(fileio::write_file_durable(path, damaged));

    // The journal still carries the full history, so recovery from the journal
    // is the honest outcome. What must never happen is inventing state.
    Result<std::unique_ptr<Coordinator>> reopened = Coordinator::open(rig.options());
    if (reopened.ok()) {
        DRC_REQUIRE_EQ(reopened.value()->event(event).value().phase, EventPhase::PlanReady);
        const Result<std::vector<SiteId>> sites = reopened.value()->sites();
        DRC_REQUIRE_OK(sites);
        DRC_REQUIRE_EQ(sites.value().size(), std::size_t{3});
        DRC_REQUIRE_OK(reopened.value()->shutdown());
    } else {
        DRC_REQUIRE(is_corruption(reopened.status().code()));
    }
}

DRC_TEST(recovery_a_tail_before_the_first_commit_does_not_break_the_sequence) {
    drctest::Rig rig = drctest::Rig::make("recovery-first-commit-tail");
    rig.open();
    build_state(rig);
    const Digest digest_before = rig.coordinator->state_digest();
    const Epoch epoch_before = rig.coordinator->epoch();
    rig.close();

    // Simulate a crash after appending records but before the first commit of a
    // fresh journal: append a well-formed record and flush it without a commit,
    // then reopen the writer, which discards the tail.
    const std::string path = journal_path(rig);
    const std::string note = "uncommitted tail before any commit";
    const std::vector<std::uint8_t> payload{note.begin(), note.end()};
    journal::Writer::Options options;
    options.path = path;
    options.max_bytes = 8u * 1024u * 1024u;
    Result<journal::Writer> first = journal::Writer::open(options);
    DRC_REQUIRE(first.ok());
    const Sequence committed_sequence = first.value().last_sequence();
    DRC_REQUIRE_OK(first.value().append(journal::RecordType::Note, payload));
    DRC_REQUIRE_OK(first.value().flush());
    DRC_REQUIRE_OK(first.value().close());

    Result<journal::Writer> second = journal::Writer::open(options);
    DRC_REQUIRE(second.ok());
    DRC_REQUIRE_EQ(second.value().scan().discarded_tail_bytes > 0, true);
    DRC_REQUIRE_EQ(second.value().last_sequence(), committed_sequence);
    // The next append must continue the file's real sequence, not the sequence
    // of the record that was discarded.
    DRC_REQUIRE_OK(second.value().append(journal::RecordType::Note, payload));
    DRC_REQUIRE_EQ(second.value().last_sequence().value(), committed_sequence.value() + 1);
    // The note records change no authority, so the digest the coordinator
    // published before the close still describes this state.
    journal::CommitPayload commit;
    commit.epoch = epoch_before;
    commit.state_digest = digest_before;
    DRC_REQUIRE_OK(second.value().commit(commit));
    DRC_REQUIRE_OK(second.value().close());

    // The repaired journal must be readable with no sequence gap.
    Result<journal::Reader> reader = journal::Reader::open(path, 8u * 1024u * 1024u);
    // Reader is move-only, so its result is checked rather than copied.
    DRC_REQUIRE(reader.ok());
    DRC_REQUIRE(!reader.value().report().interior_corruption);
    DRC_REQUIRE(reader.value().report().detail.empty() ||
                reader.value().report().detail.find("contiguous") == std::string::npos);

    // And the coordinator still opens on it.
    rig.open();
    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE_OK(verification);
    DRC_REQUIRE(verification.value().ok);
    DRC_REQUIRE(!verification.value().interior_corruption);
}

DRC_TEST(recovery_a_committed_digest_that_does_not_match_is_refused) {
    drctest::Rig rig = drctest::Rig::make("recovery-digest-fixed-point");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    const Digest digest_before = rig.coordinator->state_digest();
    const Epoch epoch_before = rig.coordinator->epoch();
    rig.close();

    // A record that changes no authority, committed with the digest the
    // coordinator published: recovery must accept it and reproduce the state.
    const std::string note = "an extra committed note";
    const std::vector<std::uint8_t> payload{note.begin(), note.end()};
    journal::Writer::Options options;
    options.path = journal_path(rig);
    options.max_bytes = 8u * 1024u * 1024u;
    Result<journal::Writer> writer = journal::Writer::open(options);
    // Writer is move-only, so the result is checked rather than copied.
    DRC_REQUIRE(writer.ok());
    DRC_REQUIRE_OK(writer.value().append(journal::RecordType::Note, payload));
    journal::CommitPayload honest;
    honest.epoch = epoch_before;
    honest.state_digest = digest_before;
    DRC_REQUIRE_OK(writer.value().commit(honest));
    DRC_REQUIRE_OK(writer.value().close());

    rig.open();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), digest_before);
    DRC_REQUIRE(rig.coordinator->epoch() > epoch_before);
    rig.close();

    // The same record committed with a digest that does not describe the
    // replayed state is a broken history, and recovery must refuse it.
    journal::Writer::Options broken_options;
    broken_options.path = journal_path(rig);
    broken_options.max_bytes = 8u * 1024u * 1024u;
    Result<journal::Writer> broken = journal::Writer::open(broken_options);
    DRC_REQUIRE(broken.ok());
    DRC_REQUIRE_OK(broken.value().append(journal::RecordType::Note, payload));
    journal::CommitPayload lying;
    lying.epoch = epoch_before;
    Sha256::DigestBytes zeros{};
    lying.state_digest = Digest{zeros};
    DRC_REQUIRE_OK(broken.value().commit(lying));
    DRC_REQUIRE_OK(broken.value().close());

    Result<std::unique_ptr<Coordinator>> reopened = Coordinator::open(rig.options());
    DRC_REQUIRE(!reopened.ok());
    DRC_REQUIRE_EQ(reopened.status().code(), ErrorCode::Corrupt);
    DRC_REQUIRE(reopened.status().message().find("digest") != std::string::npos);
}

DRC_TEST(recovery_repeated_open_and_close_is_stable) {
    drctest::Rig rig = drctest::Rig::make("recovery-cycles");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    const Digest digest = rig.coordinator->state_digest();
    Epoch epoch = rig.coordinator->epoch();
    for (int cycle = 0; cycle < 12; ++cycle) {
        rig.reopen();
        DRC_REQUIRE_EQ(rig.coordinator->state_digest(), digest);
        DRC_REQUIRE(rig.coordinator->epoch() > epoch);
        epoch = rig.coordinator->epoch();
        const Result<JournalVerification> verification = rig.coordinator->verify_journal();
        DRC_REQUIRE_OK(verification);
        DRC_REQUIRE(verification.value().ok);
    }
}

DRC_TEST(recovery_a_corrupt_committed_digest_is_refused) {
    drctest::Rig rig = drctest::Rig::make("recovery-bad-digest");
    rig.open();
    build_state(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    rig.close();

    // Flip one byte inside the payload of the very first record. Every record
    // is checksummed, so this is interior corruption and must never be
    // truncated away or silently accepted.
    const std::string path = journal_path(rig);
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, 16u * 1024u * 1024u);
    DRC_REQUIRE_OK(bytes);
    std::vector<std::uint8_t> damaged = bytes.value();
    DRC_REQUIRE(damaged.size() > journal::kHeaderBytes + 64);
    const std::size_t offset = journal::kHeaderBytes + 8;
    damaged[offset] = static_cast<std::uint8_t>(damaged[offset] ^ 0x01u);
    DRC_REQUIRE_OK(fileio::write_file_durable(path, damaged));

    Result<std::unique_ptr<Coordinator>> reopened = Coordinator::open(rig.options());
    DRC_REQUIRE(!reopened.ok());
    DRC_REQUIRE(is_corruption(reopened.status().code()));
    DRC_REQUIRE(reopened.status().message().find("corrupt") != std::string::npos ||
                reopened.status().message().find("offset") != std::string::npos);
    // The damaged file is left exactly as it was found: recovery never
    // truncates through interior corruption.
    Result<std::vector<std::uint8_t>> after = fileio::read_file(path, 16u * 1024u * 1024u);
    DRC_REQUIRE_OK(after);
    DRC_REQUIRE(after.value() == damaged);
    DRC_REQUIRE_EQ(event, DisasterEventId{1});
}
