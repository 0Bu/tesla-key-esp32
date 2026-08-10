cmake_minimum_required(VERSION 3.16)

# This firmware has one project component. Keep optional ESP-IDF components out of the build
# graph; everything main actually needs is pulled in through REQUIRES or idf_component.yml.
set(COMPONENTS main)

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

project(tesla-key-esp32)

# yoziru/tesla-ble v5.1.1 detects replayed CarServer response counters but continues
# processing those frames. That lets an old response refresh a cache callback or complete
# the next FIFO command. Apply the small, reviewable repository-owned fix after IDF's
# Component Manager has materialised the pinned dependency and before any source compiles.
execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/apply-tesla-ble-patches.sh"
    RESULT_VARIABLE TESLA_BLE_PATCH_RESULT
)
if(NOT TESLA_BLE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply the tesla-ble anti-replay patch")
endif()
