cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The yoziru/tesla-ble dependency uses %u for uint32_t, which on the Xtensa
# toolchain is 'unsigned long' — GCC 14 flags this as -Werror=format. The library
# (in managed_components/, not editable) compiles fine functionally, so downgrade
# this specific format warning from an error for the whole build.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format" APPEND)

project(tesla-key-esp32)
