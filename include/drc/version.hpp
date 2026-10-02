#pragma once

#include <cstdint>
#include <string_view>

// Single source of truth for the library version. The CMake project version is
// kept in step by unit_version_matches_the_configured_package_version, which
// compares this header against the version the build was configured with.
#define DRC_VERSION_MAJOR 1
#define DRC_VERSION_MINOR 0
#define DRC_VERSION_PATCH 1

namespace drc {

inline constexpr std::uint32_t kVersionMajor = DRC_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = DRC_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = DRC_VERSION_PATCH;

[[nodiscard]] constexpr std::string_view version_string() noexcept {
    return "1.0.1";
}

}  // namespace drc
