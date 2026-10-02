cmake_minimum_required(VERSION 3.25)

# Standalone project: it is configured against an installed prefix and never
# against this repository's build tree.
project(DrcDownstreamConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(DisasterRecoveryCoordinator 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE DisasterRecoveryCoordinator::disaster_recovery_coordinator)

if(MSVC)
    target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
