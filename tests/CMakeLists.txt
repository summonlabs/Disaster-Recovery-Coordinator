file(GLOB DRC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/suite_*.cpp")

add_executable(drc_tests test_main.cpp ${DRC_TEST_SOURCES})
target_include_directories(drc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drc_tests PRIVATE DisasterRecoveryCoordinator::disaster_recovery_coordinator)

# The multiprocess suites need the real executables, not a path guess.
if(TARGET drcctl)
    target_compile_definitions(drc_tests PRIVATE DRC_TOOLS_DIR="$<TARGET_FILE_DIR:drcctl>")
endif()

if(MSVC)
    target_compile_options(drc_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(DRC_WARNINGS_AS_ERRORS)
        target_compile_options(drc_tests PRIVATE /WX)
    endif()
else()
    target_compile_options(drc_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(DRC_WARNINGS_AS_ERRORS)
        target_compile_options(drc_tests PRIVATE -Werror)
    endif()
endif()

# One CTest entry per area so a failure names the area. No timeouts: a hang is
# a defect to diagnose, not something to kill.
foreach(area unit integration e2e property adversarial recovery multiprocess concurrency fencing cli)
    add_test(NAME ${area} COMMAND drc_tests --filter ${area}_)
    set_tests_properties(${area} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(DRC_ENABLE_MSVC_ASAN)
        set_tests_properties(${area} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1")
    endif()
endforeach()
