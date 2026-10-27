cmake_minimum_required(VERSION 3.16)

# This firmware has one project component. Keep optional ESP-IDF components out of the build
# graph; everything main actually needs is pulled in through REQUIRES or idf_component.yml.
set(COMPONENTS main)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/admit-dependency-sources.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Component Manager lockfiles encode the selected chip target. A single shared lockfile would be
# rewritten every time CI moves from esp32 to s3/c3/c6, so each supported target owns one immutable
# resolution. scripts/ci-build-all.sh verifies that a normal build never changes it.
idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.${IDF_TARGET}")

# The yoziru/tesla-ble dependency uses %u for uint32_t, which on the Xtensa
# toolchain is 'unsigned long' — GCC 14 flags this as -Werror=format. The library
# (in managed_components/, not editable) compiles fine functionally, so downgrade
# this specific format warning from an error for the whole build.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format" APPEND)

# TESLA_BUILD_ROLE_BEGIN
set(TESLA_BUILD_ROLE "application" CACHE STRING "Closed application or config-repair role")
if(TESLA_BUILD_ROLE STREQUAL "application")
    project(tesla-key-esp32)
elseif(TESLA_BUILD_ROLE STREQUAL "config-repair")
    project(tesla-key-config-repair)
else()
    message(FATAL_ERROR "Unknown firmware build role")
endif()
if(TESLA_BUILD_ROLE STREQUAL "config-repair")
    string(LENGTH "${PROJECT_VER}" TESLA_REPAIR_VERSION_LENGTH)
    if(NOT PROJECT_VER MATCHES "^cfgrepair-[0-9a-f]+$" OR NOT TESLA_REPAIR_VERSION_LENGTH EQUAL 26)
        message(FATAL_ERROR "Maintenance version must be its separate source-fingerprint identity")
    endif()
    if(NOT CONFIG_TESLA_CONFIG_REPAIR_USB)
        message(FATAL_ERROR "Maintenance role requires its explicit configuration")
    endif()
else()
    if(CONFIG_TESLA_CONFIG_REPAIR_USB)
        message(FATAL_ERROR "Maintenance configuration cannot build an application artifact")
    endif()
endif()
# TESLA_BUILD_ROLE_END

tesla_bind_dependency_admission()

# Exclude library debug/verbose argument evaluation and formatting from firmware builds.
idf_component_get_property(TESLA_BLE_COMPONENT_LIB yoziru__tesla-ble COMPONENT_LIB)
target_compile_definitions(${TESLA_BLE_COMPONENT_LIB} PRIVATE TESLA_BLE_LOG_LEVEL=2)

# yoziru/tesla-ble v6.3.0 binds Parental Controls actions into the VehicleAction oneof
# that this firmware never sends (trimmed to preserve target flash budget), and pulls
# in redundant force_update_session dead code on counter replay instead of aligning with
# signer.go. Key regeneration and BLE orchestration are handled natively in the firmware.
# Apply the ordered, reviewable repository-owned patch series after Component Manager
# has materialised the pinned dependency and before any source compiles.
# TESLA_PATCH_STATE_BEGIN
execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/apply-tesla-ble-patches.sh"
    RESULT_VARIABLE TESLA_BLE_PATCH_RESULT
)
if(NOT TESLA_BLE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply the tesla-ble patch series")
endif()

# Reconfigure before compiling an edited postimage or verifier. CONFIGURE_DEPENDS also watches
# inventories, so a newly added patch/lock cannot evade a list containing only existing leaves.
execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/apply-tesla-ble-patches.sh" --list-inputs
    OUTPUT_VARIABLE TESLA_BLE_CONFIGURE_INPUTS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE TESLA_BLE_INPUT_RESULT
)
if(NOT TESLA_BLE_INPUT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to verify the tesla-ble configure inputs")
endif()
string(REPLACE "\n" ";" TESLA_BLE_CONFIGURE_INPUTS "${TESLA_BLE_CONFIGURE_INPUTS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TESLA_BLE_CONFIGURE_INPUTS})
file(GLOB TESLA_BLE_INPUT_INVENTORY CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/patches/tesla-ble/*"
    "${CMAKE_SOURCE_DIR}/dependencies.lock.*")
# TESLA_PATCH_STATE_END
