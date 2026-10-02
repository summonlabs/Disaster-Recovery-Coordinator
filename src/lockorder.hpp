#pragma once

#include <atomic>
#include <cstdint>

// The coordinator has exactly one state lock and one bounded receipt mailbox.
// Lock ordering is encoded here rather than left to convention: a thread that
// already holds a lock at level N and acquires a lock at level <= N records a
// violation. Levels only ever increase, so no cycle can exist, and the counter
// is exposed through the coordinator statistics.
namespace drc::internal {

enum class LockLevel : int {
    None = 0,
    Engine = 1,    // coordinator state (shared_mutex)
    Mailbox = 2,   // receipt queue
    Endpoint = 3,  // per-participant request/response serialisation
};

[[nodiscard]] inline const char* to_string(LockLevel level) noexcept {
    switch (level) {
        case LockLevel::None: return "none";
        case LockLevel::Engine: return "engine";
        case LockLevel::Mailbox: return "mailbox";
        case LockLevel::Endpoint: return "endpoint";
    }
    return "unknown";
}

// MinGW clang 19 (WinLibs) cannot run thread_local storage at all: a ten-line
// program that reads or writes a thread_local int dies with an access violation
// (0xC0000005) under that toolchain, with and without -fno-emulated-tls, while
// the same program built by MSVC or MinGW GCC runs correctly. Per-thread
// nesting depth is exactly what a thread_local is for, so on that toolchain the
// tracker keeps no per-thread state and reports no violations rather than
// crashing. Everything else — the lock order itself, the guards, and the
// acquisition sequence — is unchanged, and the tracker is exercised for real by
// MSVC and GCC on Windows and by GCC and clang on Linux.
#if defined(__MINGW32__) && defined(__clang__)
#define DRC_LOCK_ORDER_TRACKING 0
#else
#define DRC_LOCK_ORDER_TRACKING 1
#endif

class LockOrder {
public:
    // Thread-local depth, so two threads never observe each other's nesting.
    [[nodiscard]] static int current_level() noexcept;
    static void enter(LockLevel level, std::atomic<std::uint64_t>* violations) noexcept;
    static void leave() noexcept;

    // False when the toolchain cannot support per-thread depth (see above).
    [[nodiscard]] static constexpr bool tracking_available() noexcept {
        return DRC_LOCK_ORDER_TRACKING != 0;
    }

#if DRC_LOCK_ORDER_TRACKING
private:
    static thread_local int level_;
#endif
};

// Acquires a level for the lifetime of the guard. A violation is counted and,
// in a build without NDEBUG, also asserted: an unreported inversion is worse
// than a loud one.
class LockOrderGuard {
public:
    LockOrderGuard(LockLevel level, std::atomic<std::uint64_t>* violations)
        : violations_(violations) {
        LockOrder::enter(level, violations_);
    }

    LockOrderGuard(const LockOrderGuard&) = delete;
    LockOrderGuard& operator=(const LockOrderGuard&) = delete;

    ~LockOrderGuard() { LockOrder::leave(); }

private:
    std::atomic<std::uint64_t>* violations_;
};

}  // namespace drc::internal
