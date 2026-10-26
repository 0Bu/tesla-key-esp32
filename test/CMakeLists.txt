# Host-side mock build — compiles the IDF-free pure logic in main/logic/ with the
# system toolchain (no ESP-IDF, no Docker, no board) so logic regressions are caught
# in seconds, locally / in CI / in a Claude Code web session. See test/README.md and
# the project AGENTS.md "Build and test entry points" section.
#
#   cmake -S test -B build_mock && cmake --build build_mock && ctest --test-dir build_mock
#
# (scripts/run-mock-tests.sh wraps these three steps.)
cmake_minimum_required(VERSION 3.16)
project(tesla_key_mock_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

enable_testing()
find_package(Threads REQUIRED)

add_executable(stack_sample_tests test_stack_sample.cpp)
target_include_directories(stack_sample_tests PRIVATE stubs ../main)
target_link_libraries(stack_sample_tests PRIVATE Threads::Threads)
add_test(NAME stack_sample_tests COMMAND stack_sample_tests)


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
target_link_libraries(nvs_storage_tests PRIVATE Threads::Threads)
add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)

# Compile real runtime glue against deterministic FreeRTOS/NVS/log stubs. This is intentionally a
# separate binary from pure logic: it proves exceptions do not cross C task trampolines and that
# diagnostic HTTP sinks run only after the shared ring mutex is released.
add_executable(runtime_boundary_tests
    test_runtime_boundaries.cpp
    test_coredump_hash.cpp
    ../main/diag_log.cpp
    ../main/stack_watch.cpp
    ../main/safe_mode.cpp
)
target_include_directories(runtime_boundary_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/coredump_stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
add_test(NAME runtime_boundary_tests COMMAND runtime_boundary_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_heap_supervisor_adapter.py
                ${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp
        DEPENDS generate_heap_supervisor_adapter.py test_runtime_boundary_contract.py
                ../main/heap_supervisor.cpp ../main/runtime_admission.cpp
                ../main/ota_update.cpp ../main/main.cpp ../main/vehicle_telemetry.cpp ../main/vehicle_ctrl.cpp
                ../main/stack_watch.hpp ../main/logic/stack_context.hpp ../main/logic/stack_sample_codec.hpp
        VERBATIM)
    add_executable(heap_supervisor_tests test_heap_supervisor.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp
        ../main/safe_mode.cpp ../main/heap_trend.cpp ../main/stack_watch.cpp)
    target_include_directories(heap_supervisor_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${CMAKE_CURRENT_SOURCE_DIR}/../main)
    add_test(NAME heap_supervisor_tests COMMAND heap_supervisor_tests)

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ota_admission_adapter.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_ota_admission_adapter.py
                ${CMAKE_CURRENT_BINARY_DIR}/ota_admission_adapter.generated.hpp
        DEPENDS generate_ota_admission_adapter.py test_runtime_boundary_contract.py
                ../main/ota_update.cpp
        VERBATIM)
    target_sources(runtime_boundary_tests PRIVATE test_ota_admission.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/ota_admission_adapter.generated.hpp)
    target_include_directories(runtime_boundary_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(runtime_boundary_tests PRIVATE TEST_OTA_ADMISSION_ADAPTER=1)
endif()

if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/background_poll_adapter.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_background_poll_adapter.py
                ${CMAKE_CURRENT_BINARY_DIR}/background_poll_adapter.generated.hpp
        DEPENDS generate_background_poll_adapter.py test_runtime_boundary_contract.py
                ../main/vehicle_telemetry.cpp
        VERBATIM)
    target_sources(runtime_boundary_tests PRIVATE test_background_poll.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/background_poll_adapter.generated.hpp)
    target_include_directories(runtime_boundary_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(runtime_boundary_tests PRIVATE TEST_BACKGROUND_POLL_ADAPTER=1)
endif()

if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/clock_adapters.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_clock_adapters.py
                ${CMAKE_CURRENT_BINARY_DIR}/clock_adapters.generated.hpp
        DEPENDS generate_clock_adapters.py test_runtime_boundary_contract.py
                ../main/main.cpp ../main/vehicle_pairing.cpp
                ../main/stack_watch.hpp ../main/logic/stack_context.hpp ../main/logic/stack_sample_codec.hpp
        VERBATIM)
    add_executable(clock_runtime_tests test_clock_runtime.cpp ../main/stack_watch.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/clock_adapters.generated.hpp)
    target_include_directories(clock_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../main ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME clock_runtime_tests COMMAND clock_runtime_tests)
    add_test(NAME runtime_boundary_contract
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_boundary_contract.py)
endif()

if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/config_commit_runtime.generated.hpp
               ${CMAKE_CURRENT_BINARY_DIR}/config_commit_owner.generated.hpp
               ${CMAKE_CURRENT_BINARY_DIR}/ota_health_task.generated.hpp
               ${CMAKE_CURRENT_BINARY_DIR}/config_portal.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_effect_reply_adapters.py
                ${CMAKE_CURRENT_BINARY_DIR}/effect_reply_adapters.generated.hpp
        DEPENDS generate_effect_reply_adapters.py test_runtime_boundary_contract.py
                fixtures/idf_ota_state_query.c fixtures/idf_ota_state_query.json
                ../esp-idf-toolchain.txt ../main/ota_update.hpp ../main/ota_update.cpp ../main/main.cpp
                ../main/logic/health_gate.hpp ../main/logic/config_commit.hpp
                ../main/ota_recovery_memory.hpp ../main/logic/ota_recovery_memory.hpp
                ../main/http_common.cpp ../main/http_handlers.hpp ../main/http_config.cpp
                ../main/http_ota.cpp ../main/http_api.cpp ../main/http_server.cpp ../main/mcp_server.cpp
                ../main/provisioning.cpp ../main/http_status.cpp ../main/diag_crash.hpp
                ../main/stack_watch.hpp ../main/logic/stack_context.hpp ../main/logic/stack_sample_codec.hpp
        VERBATIM)
    add_executable(config_commit_tests test_config_commit.cpp ../main/stack_watch.cpp fixtures/idf_ota_state_query.c
                   ${CMAKE_CURRENT_BINARY_DIR}/config_commit_runtime.generated.hpp
                   ${CMAKE_CURRENT_BINARY_DIR}/config_commit_owner.generated.hpp
                   ${CMAKE_CURRENT_BINARY_DIR}/ota_health_task.generated.hpp)
    target_include_directories(config_commit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${CMAKE_CURRENT_SOURCE_DIR}/../main ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME config_commit_tests COMMAND config_commit_tests)
endif()

# Compile actual firmware formatter/replay bodies with passive socket/cache adapters.
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/syslog_adapters.generated.hpp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_syslog_adapters.py
                ${CMAKE_CURRENT_BINARY_DIR}/syslog_adapters.generated.hpp
        DEPENDS generate_syslog_adapters.py test_runtime_boundary_contract.py ../main/syslog.cpp
        VERBATIM)
    add_executable(syslog_runtime_tests test_syslog_runtime.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/syslog_adapters.generated.hpp)
    target_include_directories(syslog_runtime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../main ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME syslog_runtime_tests COMMAND syslog_runtime_tests)
endif()

# Whole actual task bodies compile in private temporary fixtures at test execution time.
# Every run regenerates from current source; this is not an SDK/registration/resource test.
if(Python3_Interpreter_FOUND)
    add_test(NAME f137_task_host_contract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_f137_host.py
                --source ${CMAKE_CURRENT_SOURCE_DIR}/.. --controls --execute-controlled-host-only)
    set_tests_properties(f137_task_host_contract PROPERTIES TIMEOUT 120)
endif()

# Actual crash shell with passive platform/PSA I/O; host facades do not prove IDF ABI.
foreach(crash_arch IN ITEMS xtensa riscv)
    set(crash_target crash_runtime_${crash_arch}_tests)
    add_executable(${crash_target} test_crash_runtime.cpp)
    target_include_directories(${crash_target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/crash_stubs
        ${CMAKE_CURRENT_SOURCE_DIR}/coredump_stubs
        ${CMAKE_CURRENT_SOURCE_DIR}/../main)
    if(crash_arch STREQUAL "riscv")
        target_compile_definitions(${crash_target} PRIVATE HOST_CRASH_RISCV=1)
    endif()
    add_test(NAME ${crash_target} COMMAND ${crash_target})
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/config_repair_backend.cmake)
