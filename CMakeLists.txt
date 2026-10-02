cmake_minimum_required(VERSION 3.25)

project(DisasterRecoveryCoordinator
    VERSION 1.0.1
    DESCRIPTION "DCCP boundary: disaster-recovery orchestration across complete facilities"
    HOMEPAGE_URL "https://github.com/summonlabs/Disaster-Recovery-Coordinator"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(DRC_BUILD_TESTS "Build the Disaster Recovery Coordinator test suites" ON)
option(DRC_BUILD_TOOLS "Build the Disaster Recovery Coordinator command line tools" ON)
option(DRC_BUILD_BENCHMARKS "Build the Disaster Recovery Coordinator benchmarks" ON)
option(DRC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(DRC_ENABLE_SANITIZERS "Enable AddressSanitizer/UBSan where the toolchain supports it" OFF)
# MSVC ships an AddressSanitizer runtime; GCC/Clang on Windows usually do not.
# This option is separate because the flags are entirely different, and because
# a Windows sanitizer run is only meaningful where the runtime is really present.
option(DRC_ENABLE_MSVC_ASAN "Enable the MSVC AddressSanitizer (/fsanitize=address)" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(disaster_recovery_coordinator
    src/canonical.cpp
    src/codec.cpp
    src/digest.cpp
    src/effect.cpp
    src/engine.cpp
    src/error.cpp
    src/evidence.cpp
    src/fileio.cpp
    src/lockfile.cpp
    src/lockorder.cpp
    src/plan.cpp
    src/ports.cpp
    src/process.cpp
    src/state.cpp
    src/time.cpp
    src/journal.cpp
    src/checkpoint.cpp
    src/recovery.cpp
    src/advance.cpp
    src/model.cpp)

add_library(DisasterRecoveryCoordinator::disaster_recovery_coordinator ALIAS disaster_recovery_coordinator)

target_include_directories(disaster_recovery_coordinator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(disaster_recovery_coordinator PUBLIC cxx_std_20)

if(WIN32)
    target_compile_definitions(disaster_recovery_coordinator PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(disaster_recovery_coordinator PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(disaster_recovery_coordinator PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(DRC_WARNINGS_AS_ERRORS)
        target_compile_options(disaster_recovery_coordinator PRIVATE /WX)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(disaster_recovery_coordinator PRIVATE Threads::Threads)
    target_compile_options(disaster_recovery_coordinator PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(DRC_WARNINGS_AS_ERRORS)
        target_compile_options(disaster_recovery_coordinator PRIVATE -Werror)
    endif()
endif()

if(DRC_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(disaster_recovery_coordinator PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(disaster_recovery_coordinator PUBLIC -fsanitize=address,undefined)
endif()

if(DRC_ENABLE_MSVC_ASAN AND MSVC)
    # /Zi keeps C5072 (ASAN without debug information) away, which would
    # otherwise be an error under /WX.
    target_compile_options(disaster_recovery_coordinator PUBLIC /fsanitize=address /Zi)
    target_link_options(disaster_recovery_coordinator PUBLIC /INCREMENTAL:NO)
endif()

if(DRC_ENABLE_MSVC_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "DRC_ENABLE_MSVC_ASAN is only meaningful with MSVC; use DRC_ENABLE_SANITIZERS on GCC/Clang")
    endif()
    set(DRC_ASAN_TEST_ENV "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1")
endif()

if(DRC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(DRC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DRC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

install(TARGETS disaster_recovery_coordinator
    EXPORT DisasterRecoveryCoordinatorTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/drc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(DRC_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/DisasterRecoveryCoordinator)

install(EXPORT DisasterRecoveryCoordinatorTargets
    FILE DisasterRecoveryCoordinatorTargets.cmake
    NAMESPACE DisasterRecoveryCoordinator::
    DESTINATION ${DRC_CMAKE_INSTALL_DIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DisasterRecoveryCoordinatorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DisasterRecoveryCoordinatorConfig.cmake
    INSTALL_DESTINATION ${DRC_CMAKE_INSTALL_DIR})

# No ARCH_INDEPENDENT: the package installs a compiled C++ library and C++
# headers, so it is only usable by a consumer built for the same architecture
# and pointer size. ARCH_INDEPENDENT would remove exactly that check and let a
# mismatched consumer accept the package and then fail at link time.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/DisasterRecoveryCoordinatorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/DisasterRecoveryCoordinatorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/DisasterRecoveryCoordinatorConfigVersion.cmake
    DESTINATION ${DRC_CMAKE_INSTALL_DIR})

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/DisasterRecoveryCoordinator)
