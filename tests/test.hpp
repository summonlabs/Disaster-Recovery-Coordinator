#pragma once

// Minimal first-party test framework. A test asserts an invariant and prints
// the values that broke it; a failing REQUIRE aborts the test so that a cascade
// of secondary failures does not hide the first cause.

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "drc/engine.hpp"
#include "drc/error.hpp"

namespace drctest {

struct TestAbort {};

struct TestCase {
    std::string name;
    void (*fn)();
};

std::vector<TestCase>& registry();
std::uint64_t& checks();
std::uint64_t& failures();

void fail(const char* file, int line, const std::string& message);
void bump_checks();

struct Registrar {
    Registrar(const char* name, void (*fn)());
};

inline std::string text(const std::string& value) { return value; }
inline std::string text(const char* value) { return value == nullptr ? "<null>" : value; }
inline std::string text(bool value) { return value ? "true" : "false"; }
inline std::string text(std::string_view value) { return std::string{value}; }

inline std::string text(drc::ErrorCode value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::RecoveryClass value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::StepKind value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::StepState value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::PlanState value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::PlanKind value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EventPhase value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EventDisposition value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EffectDomain value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EffectKind value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EffectOutcome value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::Freshness value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::SiteAvailability value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::ReceiptDisposition value) { return std::string{drc::to_string(value)}; }
inline std::string text(drc::EvidenceKind value) { return std::string{drc::to_string(value)}; }

template <class Tag>
std::string text(const drc::Id<Tag>& value) {
    return value.to_string();
}

template <class Tag>
std::string text(const drc::Counter<Tag>& value) {
    return value.to_string();
}

inline std::string text(const drc::Digest& value) { return value.to_hex(); }
inline std::string text(const drc::Status& value) { return value.to_string(); }

// Anything with a stream operator prints itself; anything else prints a
// placeholder rather than breaking the test build.
template <class T>
std::string text(const T& value) {
    if constexpr (requires(std::ostringstream& stream, const T& item) { stream << item; }) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<unprintable>";
    }
}

inline const drc::Status& error_of(const drc::Status& status) { return status; }

template <class T>
const drc::Status& error_of(const drc::Result<T>& result) {
    return result.status();
}

inline drc::ErrorCode code_of(const drc::Status& status) { return status.code(); }

template <class T>
drc::ErrorCode code_of(const drc::Result<T>& result) {
    return result.code();
}

}  // namespace drctest

#define DRC_TEST(name)                                                          \
    static void drc_test_##name();                                              \
    static const ::drctest::Registrar drc_registrar_##name(#name, &drc_test_##name); \
    static void drc_test_##name()

#define DRC_CHECK(condition)                                                                   \
    do {                                                                                       \
        ::drctest::bump_checks();                                                              \
        if (!(condition)) {                                                                    \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{"check failed: "} + #condition);                       \
        }                                                                                      \
    } while (false)

#define DRC_REQUIRE(condition)                                                                 \
    do {                                                                                       \
        ::drctest::bump_checks();                                                              \
        if (!(condition)) {                                                                    \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{"requirement failed: "} + #condition);                  \
            throw ::drctest::TestAbort{};                                                      \
        }                                                                                      \
    } while (false)

// The operands are copied, not bound to a reference: binding a reference to
// the value of a temporary Result is a dangling-reference warning under GCC
// and a real dangling reference under AddressSanitizer.
#define DRC_REQUIRE_EQ(actual, expected)                                                       \
    do {                                                                                       \
        ::drctest::bump_checks();                                                              \
        const auto drc_a__ = (actual);                                                         \
        const auto drc_b__ = (expected);                                                       \
        if (!(drc_a__ == drc_b__)) {                                                           \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{"expected "} + #actual + " == " + #expected +          \
                                ", got " + ::drctest::text(drc_a__) + " vs " +                 \
                                ::drctest::text(drc_b__));                                     \
            throw ::drctest::TestAbort{};                                                      \
        }                                                                                      \
    } while (false)

#define DRC_REQUIRE_OK(expression)                                                             \
    do {                                                                                       \
        ::drctest::bump_checks();                                                              \
        auto drc_r__ = (expression);                                                           \
        if (!drc_r__.ok()) {                                                                   \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{#expression " failed: "} +                             \
                                ::drctest::error_of(drc_r__).to_string());                     \
            throw ::drctest::TestAbort{};                                                      \
        }                                                                                      \
    } while (false)

#define DRC_REQUIRE_ERR(expression, expected_code)                                             \
    do {                                                                                       \
        ::drctest::bump_checks();                                                              \
        auto drc_r__ = (expression);                                                           \
        if (drc_r__.ok()) {                                                                    \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{#expression " unexpectedly succeeded; expected "} +    \
                                ::drctest::text(expected_code));                               \
            throw ::drctest::TestAbort{};                                                      \
        }                                                                                      \
        if (::drctest::code_of(drc_r__) != (expected_code)) {                                  \
            ::drctest::fail(__FILE__, __LINE__,                                                \
                            std::string{#expression " failed with "} +                         \
                                ::drctest::text(::drctest::code_of(drc_r__)) +                 \
                                " instead of " + ::drctest::text(expected_code) + ": " +       \
                                ::drctest::error_of(drc_r__).to_string());                     \
            throw ::drctest::TestAbort{};                                                      \
        }                                                                                      \
    } while (false)
