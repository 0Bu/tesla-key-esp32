# Host-side mock build — compiles the IDF-free pure logic in main/logic/ with the
# system toolchain (no ESP-IDF, no Docker, no board) so logic regressions are caught
# in seconds, locally / in CI / in a Claude Code web session. See test/README.md and
# the project CLAUDE.md "Host-side mock build" section.
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
