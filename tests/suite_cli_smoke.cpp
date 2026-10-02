// The console as a product: what a user sees when they run drcctl. Every test
// drives the real executable through drc::process::Child and reads the text it
// printed. Nothing is skipped on a non-zero exit: a failing command is a
// failure, and a hang would be reported as a hang (there is no timeout here).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "drc/process.hpp"
#include "drc/version.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;
// The captured-console result type lives in the shared harness.
using drctest::ConsoleResult;

namespace {

[[nodiscard]] std::filesystem::path leaf(const std::string& directory, const std::string& name) {
    return std::filesystem::path(directory) / name;
}

void write_text(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    stream.close();
}

[[nodiscard]] bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// The console, run with its output captured. The shell integration lives in the
// shared harness so the same test drives the real executable on every platform.
[[nodiscard]] drctest::ConsoleResult run_console(const std::string& scratch,
                                                const std::string& tag,
                                                const std::vector<std::string>& arguments) {
    std::vector<std::string> command{drctest::tool_path("drcctl")};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return drctest::run_captured(scratch, tag, command);
}

// The whole lifecycle as one script: two failure domains, three sites, two
// protected obligations, capacity and readiness evidence, and in-process
// synthetic participants for all four effect domains.
[[nodiscard]] std::string lifecycle_script() {
    return R"(# topology
domain --id 1 --name domain-a
domain --id 2 --name domain-b
site --id 1 --domain 1 --name site-a1 --capacity 100
site --id 2 --domain 1 --name site-a2 --capacity 100
site --id 3 --domain 2 --name site-b1 --capacity 1000
obligation --id 10 --home 1 --capacity 10 --class safety_critical --name auth
obligation --id 11 --home 2 --capacity 20 --class protected --name orders
# evidence
capability --site 3 --generation 1 --capacity 500
readiness --site 1 --generation 2 --check power=pass --check network=pass
readiness --site 2 --generation 2 --check power=pass --check network=pass
# participants
endpoint --domain site_control_plane --id 1 --name scp
endpoint --domain placement_reservation_capacity --id 2 --name prc
endpoint --domain asi_execution_recovery --id 3 --name asi
endpoint --domain dfi_network_recovery --id 4 --name dfi
# incident
declare --sites 1,2 --by operator --reason site-a-is-down
plan --event 1
begin --event 1
advance --event 1 --rounds 8 --dispatches 64 --repeat 8
# restoration
restore --event 1
advance --event 1 --rounds 8 --dispatches 64 --repeat 4
return --event 1 --site 1
advance --event 1 --rounds 8 --dispatches 64 --repeat 4
return --event 1 --site 2
advance --event 1 --rounds 8 --dispatches 64 --repeat 4
close --event 1
status --event 1
)";
}

// Opens the directory in this process and reports what the console left
// behind. The coordinator is shut down before returning so the console may be
// run again on the same directory.
struct ObservedState {
    Digest digest;
    std::uint64_t epoch = 0;
    EventPhase phase = EventPhase::Declared;
    EventDisposition disposition = EventDisposition::Active;
    std::size_t returned_sites = 0;
    bool has_event = false;
};

[[nodiscard]] ObservedState observe(const std::string& directory) {
    CoordinatorOptions options;
    options.directory = directory;
    options.owner = "cli-smoke";
    options.clock = std::make_shared<ManualClock>();
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(std::move(options));
    DRC_REQUIRE(opened.ok());
    ObservedState state;
    state.digest = opened.value()->state_digest();
    state.epoch = opened.value()->epoch().value();
    const Result<std::vector<DisasterEventId>> events = opened.value()->events();
    DRC_REQUIRE(events.ok());
    if (!events.value().empty()) {
        const Result<DisasterEvent> event = opened.value()->event(events.value().front());
        DRC_REQUIRE(event.ok());
        state.has_event = true;
        state.phase = event.value().phase;
        state.disposition = event.value().disposition;
        state.returned_sites = event.value().returned_sites.size();
    }
    DRC_REQUIRE_OK(opened.value()->shutdown());
    return state;
}

[[nodiscard]] std::string mask_epoch_lines(const std::string& text) {
    std::string masked;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t end = text.find('\n', offset);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        const std::string line = text.substr(offset, stop - offset);
        if (line.rfind("epoch ", 0) != 0) {
            masked.append(line);
            masked.push_back('\n');
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return masked;
}

[[nodiscard]] std::size_t count_epoch_lines(const std::string& text) {
    std::size_t lines = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t end = text.find('\n', offset);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        if (text.substr(offset, stop - offset).rfind("epoch ", 0) == 0) {
            lines += 1;
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return lines;
}

}  // namespace

DRC_TEST(cli_version_and_usage_are_honest) {
    drctest::TempDir scratch("cli-usage");

    // No arguments at all: the console explains itself and refuses to guess.
    const ConsoleResult bare = run_console(scratch.path, "bare", {});
    DRC_REQUIRE_EQ(bare.exit_code, 2);
    DRC_REQUIRE(contains(bare.output, "usage: drcctl"));
    DRC_REQUIRE(contains(bare.output, "drcctl " + std::string{drc::version_string()}));
    DRC_REQUIRE(contains(bare.output, "--dir DIR"));
    DRC_REQUIRE(contains(bare.output, "commands:"));

    // A command without a journal directory is refused the same way.
    const ConsoleResult without_dir = run_console(scratch.path, "without-dir", {"verify"});
    DRC_REQUIRE_EQ(without_dir.exit_code, 2);
    DRC_REQUIRE(contains(without_dir.output, "usage: drcctl"));
}

DRC_TEST(cli_script_runs_the_full_lifecycle) {
    drctest::TempDir directory("cli-lifecycle");
    const std::filesystem::path script = leaf(directory.path, "life.txt");
    write_text(script, lifecycle_script());

    const ConsoleResult run =
        run_console(directory.path, "life",
                    {"--dir", directory.path, "--clock", "manual", "--script", script.string()});
    DRC_REQUIRE_EQ(run.exit_code, 0);

    // The deterministic lines a script author relies on.
    DRC_REQUIRE(contains(run.output, "ok declare event="));
    DRC_REQUIRE(contains(run.output, "ok plan plan="));
    DRC_REQUIRE(contains(run.output, "ok begin"));
    DRC_REQUIRE(contains(run.output, "advance event="));
    DRC_REQUIRE(contains(run.output, "phase=stabilized"));
    DRC_REQUIRE(contains(run.output, "ok restore"));
    DRC_REQUIRE(contains(run.output, "ok return"));
    DRC_REQUIRE(contains(run.output, "ok close"));
    DRC_REQUIRE(contains(run.output, "phase=closed"));

    // The console's work is real state, not just text.
    const ObservedState state = observe(directory.path);
    DRC_REQUIRE(state.has_event);
    DRC_REQUIRE_EQ(state.phase, EventPhase::Closed);
    DRC_REQUIRE_EQ(state.disposition, EventDisposition::Closed);
    DRC_REQUIRE_EQ(state.returned_sites, std::size_t{2});
}

DRC_TEST(cli_verify_and_dump_report_the_real_state) {
    drctest::TempDir directory("cli-state");
    const std::filesystem::path script = leaf(directory.path, "life.txt");
    write_text(script, lifecycle_script());
    const ConsoleResult prepared =
        run_console(directory.path, "life",
                    {"--dir", directory.path, "--clock", "manual", "--script", script.string()});
    DRC_REQUIRE_EQ(prepared.exit_code, 0);

    // What a coordinator opened in this process sees on the same directory.
    const ObservedState observed = observe(directory.path);

    const ConsoleResult dump =
        run_console(directory.path, "dump", {"--dir", directory.path, "dump"});
    DRC_REQUIRE_EQ(dump.exit_code, 0);
    DRC_REQUIRE(contains(dump.output, "digest " + observed.digest.to_hex()));
    DRC_REQUIRE(contains(dump.output, "event id=1 phase=closed"));
    DRC_REQUIRE(contains(dump.output, "obligation id=10"));
    DRC_REQUIRE(contains(dump.output, "site id=3"));
    // Every open fences the previous incarnation, so the console that opened
    // after this process reports the next epoch for the same state.
    DRC_REQUIRE(contains(dump.output, "epoch " + std::to_string(observed.epoch + 1)));

    const ConsoleResult verify =
        run_console(directory.path, "verify", {"--dir", directory.path, "verify"});
    DRC_REQUIRE_EQ(verify.exit_code, 0);
    DRC_REQUIRE(contains(verify.output, "verify ok=1"));
    DRC_REQUIRE(contains(verify.output, "torn_tail=0"));
    DRC_REQUIRE(contains(verify.output, "interior_corruption=0"));

    // The console's two opens fenced this process's: the digest is unchanged
    // and the epoch counts coordinator incarnations.
    const ObservedState again = observe(directory.path);
    DRC_REQUIRE_EQ(again.digest, observed.digest);
    DRC_REQUIRE_EQ(again.epoch, observed.epoch + 3);
    DRC_REQUIRE_EQ(again.phase, observed.phase);
}

DRC_TEST(cli_rejects_unknown_command_and_missing_arguments) {
    drctest::TempDir directory("cli-errors");

    // An unknown command is refused with non-zero status and an error line that
    // names the command.
    const ConsoleResult unknown =
        run_console(directory.path, "unknown", {"--dir", directory.path, "frobnicate"});
    DRC_REQUIRE_EQ(unknown.exit_code, 1);
    DRC_REQUIRE(!unknown.output.empty());
    DRC_REQUIRE(contains(unknown.output, "error frobnicate " +
                                             std::string{to_string(ErrorCode::Unsupported)}));
    DRC_REQUIRE(contains(unknown.output, "frobnicate"));

    // A command missing a required flag fails with the documented error code
    // and names the command it refused.
    const ConsoleResult missing =
        run_console(directory.path, "missing", {"--dir", directory.path, "site", "--id", "1"});
    DRC_REQUIRE_EQ(missing.exit_code, 1);
    DRC_REQUIRE(
        contains(missing.output, "error site " + std::string{to_string(ErrorCode::Invalid)}));

    // The same command with every flag present succeeds when it is run from a
    // script, which is the dispatch path the flags are meant to reach.
    const std::filesystem::path script = leaf(directory.path, "site.txt");
    write_text(script, "domain --id 1 --name domain-a\nsite --id 1 --domain 1 --name s1\n");
    const ConsoleResult present =
        run_console(directory.path, "present",
                    {"--dir", directory.path, "--script", script.string()});
    DRC_REQUIRE_EQ(present.exit_code, 0);
    DRC_REQUIRE(contains(present.output, "ok site"));

    // A referenced object that does not exist is a documented NotFound.
    const std::filesystem::path capability = leaf(directory.path, "capability.txt");
    write_text(capability, "capability --site 99 --generation 1 --capacity 10\n");
    const ConsoleResult not_found =
        run_console(directory.path, "not-found",
                    {"--dir", directory.path, "--script", capability.string()});
    DRC_REQUIRE_EQ(not_found.exit_code, 1);
    DRC_REQUIRE(contains(not_found.output,
                         "error capability " + std::string{to_string(ErrorCode::NotFound)}));
}

// The usage text documents "COMMAND [--key value ...]". Options written after
// the command name belong to the command; they must not be swallowed as global
// options and dropped, which makes every flag-bearing command unusable from the
// command line.
DRC_TEST(cli_command_flags_reach_the_command) {
    drctest::TempDir directory("cli-flags");
    // The site names a failure domain, so the domain has to exist first.
    const ConsoleResult domain =
        run_console(directory.path, "flags-domain",
                    {"--dir", directory.path, "domain", "--id", "1", "--name", "domain-a"});
    DRC_REQUIRE_EQ(domain.exit_code, 0);
    DRC_REQUIRE(contains(domain.output, "ok domain"));

    const ConsoleResult site =
        run_console(directory.path, "flags",
                    {"--dir", directory.path, "site", "--id", "1", "--domain", "1",
                     "--name", "s1", "--capacity", "100"});
    DRC_REQUIRE_EQ(site.exit_code, 0);
    DRC_REQUIRE(contains(site.output, "ok site"));

    // The command really ran with the values it was given.
    const ConsoleResult dump =
        run_console(directory.path, "flags-dump", {"--dir", directory.path, "dump"});
    DRC_REQUIRE_EQ(dump.exit_code, 0);
    DRC_REQUIRE(contains(dump.output, "site id=1 name=s1 domain=1 capacity=100"));
}

DRC_TEST(cli_is_repeatable) {
    drctest::TempDir directory("cli-repeat");
    const std::filesystem::path lifecycle = leaf(directory.path, "life.txt");
    write_text(lifecycle, lifecycle_script());
    const ConsoleResult prepared =
        run_console(directory.path, "life",
                    {"--dir", directory.path, "--clock", "manual", "--script", lifecycle.string()});
    DRC_REQUIRE_EQ(prepared.exit_code, 0);

    // Read-only commands only: nothing here may change authoritative state.
    const std::filesystem::path read_only = leaf(directory.path, "read.txt");
    write_text(read_only, "dump\nstatus --event 1\n");

    // The same directory twice: identical apart from the coordinator epoch,
    // which counts incarnations by design.
    const ConsoleResult first =
        run_console(directory.path, "same-a", {"--dir", directory.path, "--script", read_only.string()});
    const ConsoleResult second =
        run_console(directory.path, "same-b", {"--dir", directory.path, "--script", read_only.string()});
    DRC_REQUIRE_EQ(first.exit_code, 0);
    DRC_REQUIRE_EQ(second.exit_code, 0);
    DRC_REQUIRE(count_epoch_lines(first.output) > 0);
    DRC_REQUIRE_EQ(count_epoch_lines(first.output), count_epoch_lines(second.output));
    DRC_REQUIRE_EQ(mask_epoch_lines(first.output), mask_epoch_lines(second.output));

    // Two identical copies of the same directory: nothing may differ at all.
    drctest::TempDir mirrors("cli-mirrors");
    const std::filesystem::path copy_a = leaf(mirrors.path, "a");
    const std::filesystem::path copy_b = leaf(mirrors.path, "b");
    std::error_code error;
    std::filesystem::copy(directory.path, copy_a, std::filesystem::copy_options::recursive, error);
    DRC_REQUIRE(!error);
    std::filesystem::copy(directory.path, copy_b, std::filesystem::copy_options::recursive, error);
    DRC_REQUIRE(!error);
    const ConsoleResult mirrored_a =
        run_console(copy_a.string(), "mirror-a", {"--dir", copy_a.string(), "--script", read_only.string()});
    const ConsoleResult mirrored_b =
        run_console(copy_b.string(), "mirror-b", {"--dir", copy_b.string(), "--script", read_only.string()});
    DRC_REQUIRE_EQ(mirrored_a.exit_code, 0);
    DRC_REQUIRE_EQ(mirrored_b.exit_code, 0);
    DRC_REQUIRE_EQ(mirrored_a.output, mirrored_b.output);
    DRC_REQUIRE(contains(mirrored_a.output, "digest "));
    DRC_REQUIRE(contains(mirrored_a.output, "epoch "));
}
