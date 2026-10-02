#include "test.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace drctest {
namespace {

std::vector<TestCase> g_tests;
std::uint64_t g_checks = 0;
std::uint64_t g_failures = 0;

}  // namespace

std::vector<TestCase>& registry() { return g_tests; }
std::uint64_t& checks() { return g_checks; }
std::uint64_t& failures() { return g_failures; }
void bump_checks() { g_checks += 1; }

void fail(const char* file, int line, const std::string& message) {
    g_failures += 1;
    std::cout << "    FAIL " << file << ":" << line << ": " << message << "\n";
    std::cout.flush();
}

Registrar::Registrar(const char* name, void (*fn)()) {
    g_tests.push_back(TestCase{name, fn});
}

}  // namespace drctest

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--filter" && i + 1 < argc) {
            filters.emplace_back(argv[++i]);
        } else if (argument == "--list") {
            list_only = true;
        } else if (argument == "--help") {
            std::cout << "usage: drc_tests [--filter PREFIX]... [--list]\n";
            return 0;
        } else {
            std::cout << "unknown argument: " << argument << "\n";
            return 2;
        }
    }

    std::vector<drctest::TestCase>& tests = drctest::registry();
    std::size_t selected = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const drctest::TestCase& test : tests) {
        bool match = filters.empty();
        for (const std::string& filter : filters) {
            if (test.name.rfind(filter, 0) == 0 || test.name.find(filter) != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) {
            continue;
        }
        selected += 1;
        if (list_only) {
            std::cout << test.name << "\n";
            continue;
        }
        const std::uint64_t failures_before = drctest::failures();
        std::cout << "  RUN  " << test.name << "\n";
        // Flushed before the test runs: if a test aborts the process, the line
        // that names it must already be on disk, or the crash has no author.
        std::cout.flush();
        try {
            test.fn();
        } catch (const drctest::TestAbort&) {
            // The failing requirement already reported itself.
        } catch (const std::exception& error) {
            drctest::fail("<test>", 0, std::string{"unexpected exception: "} + error.what());
        } catch (...) {
            drctest::fail("<test>", 0, "unexpected non-standard exception");
        }
        if (drctest::failures() == failures_before) {
            passed += 1;
            std::cout << "  PASS " << test.name << "\n";
        } else {
            failed += 1;
            std::cout << "  FAIL " << test.name << "\n";
        }
        std::cout.flush();
    }
    if (list_only) {
        std::cout << selected << " tests\n";
        return 0;
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << drctest::checks()
              << " checks, " << selected << " of " << tests.size() << " tests selected\n";
    return failed == 0 ? 0 : 1;
}
