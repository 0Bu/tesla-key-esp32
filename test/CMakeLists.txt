# Host-side mock build — compiles the IDF-free pure logic in main/logic/ with the
# system toolchain (no ESP-IDF, no Docker, no board) so logic regressions are caught
# in seconds, locally / in CI / in a Claude Code web session. See test/README.md and
# the project .claude/CLAUDE.md "Build and test entry points" section.
#
#   cmake -S test -B build_mock && cmake --build build_mock && ctest --test-dir build_mock
#
# (scripts/run-mock-tests.sh wraps these three steps.)
cmake_minimum_required(VERSION 3.16)
project(tesla_key_mock_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

enable_testing()

add_executable(logic_tests test_logic.cpp)
# main/logic/*.hpp is included as "logic/<name>.hpp", matching the firmware include path.
target_include_directories(logic_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../main)

add_test(NAME logic_tests COMMAND logic_tests)

# Compile the real NVS adapter against a deterministic host stub. This exercises the two-call
# nvs_get_str() protocol and proves that probe/read/type/length faults never collapse to Missing.
add_executable(nvs_storage_tests
    test_nvs_storage.cpp
    ../main/nvs_storage.cpp
    ../main/config_blob.cpp
)
target_include_directories(nvs_storage_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)

# Compile real runtime glue against deterministic FreeRTOS/NVS/log stubs. This is intentionally a
# separate binary from pure logic: it proves exceptions do not cross C task trampolines and that
# diagnostic HTTP sinks run only after the shared ring mutex is released.
add_executable(runtime_boundary_tests
    test_runtime_boundaries.cpp
    ../main/diag_log.cpp
    ../main/safe_mode.cpp
)
target_include_directories(runtime_boundary_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
add_test(NAME runtime_boundary_tests COMMAND runtime_boundary_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME runtime_boundary_contract
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_boundary_contract.py)
endif()
