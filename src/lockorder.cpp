#include "lockorder.hpp"

#include <cassert>

namespace drc::internal {

#if DRC_LOCK_ORDER_TRACKING
thread_local int LockOrder::level_ = 0;

int LockOrder::current_level() noexcept {
    return level_;
}

void LockOrder::enter(LockLevel level, std::atomic<std::uint64_t>* violations) noexcept {
    const int requested = static_cast<int>(level);
    if (level_ != 0 && requested <= level_) {
        if (violations != nullptr) {
            violations->fetch_add(1);
        }
        assert(false && "lock order violation: a lock was acquired out of order");
    }
    level_ = requested;
}

void LockOrder::leave() noexcept {
    level_ = 0;
}
#else
int LockOrder::current_level() noexcept {
    return 0;
}

void LockOrder::enter(LockLevel level, std::atomic<std::uint64_t>* violations) noexcept {
    (void)level;
    (void)violations;
}

void LockOrder::leave() noexcept {}
#endif

}  // namespace drc::internal
