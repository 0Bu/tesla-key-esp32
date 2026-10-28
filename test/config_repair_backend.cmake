# Host-only actual maintenance backend/run/transport graph. No shipping role/config changes.
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "actual maintenance backend tests require host Python")
endif()
set(repair_generated ${CMAKE_CURRENT_BINARY_DIR}/config_repair_backend.generated.hpp)
add_custom_command(OUTPUT ${repair_generated} ${CMAKE_CURRENT_BINARY_DIR}/config_repair_backend.generated.json
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_config_repair_backend.py ${repair_generated}
    DEPENDS generate_config_repair_backend.py ../main/config_repair.cpp ../main/config_repair_console.cpp
    VERBATIM)
foreach(repair_target IN ITEMS esp32 esp32s3 esp32c3 esp32c6)
    string(TOUPPER ${repair_target} repair_upper)
    set(repair_binary config_repair_backend_${repair_target}_tests)
    add_executable(${repair_binary} test_config_repair_backend.cpp ${repair_generated})
    target_include_directories(${repair_binary} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/config_repair_sdk
        ${CMAKE_CURRENT_SOURCE_DIR}/../main ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(${repair_binary} PRIVATE CONFIG_IDF_TARGET_${repair_upper}=1)
    foreach(repair_mode IN ITEMS backend-direct run-positive console-positive console-installed console-expired console-zero console-bad-count console-tx-deadline console-entry-installed read-time-progress)
        add_test(NAME ${repair_binary}_${repair_mode} COMMAND ${repair_binary} ${repair_mode})
        set_tests_properties(${repair_binary}_${repair_mode} PROPERTIES TIMEOUT 15)
    endforeach()
endforeach()
foreach(repair_mode IN ITEMS owner-busy owner-late raw-pending raw-late storage-init key-parse key-bits key-refresh wrong-public wrong-challenge journal-present journal-type journal-late cfg-valid cfg-missing length-drift iterator-find iterator-info iterator-next value-overlong unsupported-type hash-setup info-failure read-failure receipt-failure set-visible-failure commit-visible-failure readback-corrupt retention-change readback-substitute retention-length-drift key-journal-late)
    add_test(NAME config_repair_backend_fault_${repair_mode} COMMAND config_repair_backend_esp32s3_tests ${repair_mode})
    set_tests_properties(config_repair_backend_fault_${repair_mode} PROPERTIES TIMEOUT 15)
endforeach()
