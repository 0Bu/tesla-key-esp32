# Admit repository consumers before ESP-IDF project/component processing. The unchanged
# upstream component's later nanopb declaration sees this first global population.
foreach(TESLA_SOURCE_OVERRIDE IN ITEMS
        FETCHCONTENT_SOURCE_DIR_NANOPB
        CMAKE_PROJECT_TOP_LEVEL_INCLUDES CMAKE_PROJECT_INCLUDE_BEFORE)
    if(DEFINED ${TESLA_SOURCE_OVERRIDE} AND NOT "${${TESLA_SOURCE_OVERRIDE}}" STREQUAL "")
        message(FATAL_ERROR "Unsupported dependency source/provider override: ${TESLA_SOURCE_OVERRIDE}")
    endif()
endforeach()
if(DEFINED FETCHCONTENT_BASE_DIR AND NOT
        "${FETCHCONTENT_BASE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}/_deps")
    message(FATAL_ERROR "Unsupported FetchContent base directory")
endif()
foreach(TESLA_SOURCE_OFFLINE IN ITEMS FETCHCONTENT_FULLY_DISCONNECTED
        FETCHCONTENT_UPDATES_DISCONNECTED FETCHCONTENT_UPDATES_DISCONNECTED_NANOPB)
    if(${TESLA_SOURCE_OFFLINE})
        message(FATAL_ERROR "Dependency admission requires its own cold/warm source policy")
    endif()
endforeach()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
set(TESLA_NANOPB_ADMISSION_MODE)
if(DEFINED TESLA_NANOPB_ADMITTED_SOURCE)
    if(NOT TESLA_NANOPB_ADMITTED_SOURCE STREQUAL "${CMAKE_BINARY_DIR}/_deps/nanopb-src")
        message(FATAL_ERROR "Dependency admission cache belongs to another source path")
    endif()
    list(APPEND TESLA_NANOPB_ADMISSION_MODE --existing-only)
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/dependency_source.py"
            nanopb --root "${CMAKE_SOURCE_DIR}"
            --source "${CMAKE_BINARY_DIR}/_deps/nanopb-src" --configure-inputs ${TESLA_NANOPB_ADMISSION_MODE}
    RESULT_VARIABLE TESLA_NANOPB_ADMISSION_RESULT
    OUTPUT_VARIABLE TESLA_NANOPB_ADMISSION_INPUTS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 120
)
if(NOT "${TESLA_NANOPB_ADMISSION_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Exact nanopb source admission failed before project processing")
endif()
string(REPLACE "\n" ";" TESLA_NANOPB_ADMISSION_INPUTS "${TESLA_NANOPB_ADMISSION_INPUTS}")
list(POP_FRONT TESLA_NANOPB_ADMISSION_INPUTS TESLA_NANOPB_COMMIT)
set(TESLA_NANOPB_ADMITTED_SOURCE "${CMAKE_BINARY_DIR}/_deps/nanopb-src"
    CACHE INTERNAL "Previously admitted exact nanopb source; missing warm state is an error" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TESLA_NANOPB_ADMISSION_INPUTS})

include(FetchContent)
FetchContent_GetProperties(nanopb)
if(nanopb_POPULATED)
    message(FATAL_ERROR "nanopb was populated before repository source admission")
endif()
FetchContent_Declare(nanopb
    GIT_REPOSITORY "https://github.com/nanopb/nanopb.git"
    GIT_TAG "${TESLA_NANOPB_COMMIT}"
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/nanopb-src"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/nanopb-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
)
# The name form records global population without configuring the dependency subdirectory.
# The pinned CMake supports CMP0169; older supported CMake versions use this form directly.
if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Populate(nanopb)
if(POLICY CMP0169)
    cmake_policy(POP)
endif()
FetchContent_GetProperties(nanopb)
if(NOT nanopb_POPULATED OR NOT nanopb_SOURCE_DIR STREQUAL "${CMAKE_BINARY_DIR}/_deps/nanopb-src")
    message(FATAL_ERROR "nanopb population differs from the admitted source")
endif()

# Timestamp restoration must not bypass admission on an incremental build. Every component
# consumer waits for this always-run, read-only existing-source check, including direct target
# builds and parallel compilers. It has no consumer dependency of its own.
function(tesla_bind_dependency_admission)
    add_custom_target(tesla_dependency_source_check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/dependency_source.py"
                nanopb --root "${CMAKE_SOURCE_DIR}"
                --source "${CMAKE_BINARY_DIR}/_deps/nanopb-src" --existing-only
        VERBATIM
    )
    idf_build_get_property(TESLA_SOURCE_COMPONENTS BUILD_COMPONENTS)
    foreach(TESLA_SOURCE_COMPONENT IN LISTS TESLA_SOURCE_COMPONENTS)
        idf_component_get_property(TESLA_SOURCE_COMPONENT_LIB
            "${TESLA_SOURCE_COMPONENT}" COMPONENT_LIB)
        if(TARGET "${TESLA_SOURCE_COMPONENT_LIB}")
            add_dependencies("${TESLA_SOURCE_COMPONENT_LIB}" tesla_dependency_source_check)
        endif()
    endforeach()
    # These two actual consumers must exist: Tesla compiles the nanopb C sources, while main
    # consumes their headers. The final ELF is also guarded when built directly.
    foreach(TESLA_SOURCE_REQUIRED_COMPONENT IN ITEMS main yoziru__tesla-ble)
        idf_component_get_property(TESLA_SOURCE_REQUIRED_LIB
            "${TESLA_SOURCE_REQUIRED_COMPONENT}" COMPONENT_LIB)
        if(NOT TARGET "${TESLA_SOURCE_REQUIRED_LIB}")
            message(FATAL_ERROR "Missing dependency-source consumer target")
        endif()
        add_dependencies("${TESLA_SOURCE_REQUIRED_LIB}" tesla_dependency_source_check)
    endforeach()
    if(NOT TARGET "${CMAKE_PROJECT_NAME}.elf")
        message(FATAL_ERROR "Missing dependency-source ELF consumer target")
    endif()
    add_dependencies("${CMAKE_PROJECT_NAME}.elf" tesla_dependency_source_check)
endfunction()
