"""Closed maintenance source/role contract shared by read-only build/runtime validators."""
from pathlib import Path

MAINTENANCE_PROJECT = "tesla-key-config-repair"
APPLICATION_PROJECT = "tesla-key-esp32"
TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
REQUIREMENTS_PREFIX = '# TESLA_REPAIR_REQUIREMENTS_BEGIN\nset(TESLA_REPAIR_REQUIRES)\nif(CONFIG_TESLA_CONFIG_REPAIR_USB)\n    list(APPEND TESLA_REPAIR_REQUIRES esp_driver_uart)\n    if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32C6)\n        list(APPEND TESLA_REPAIR_REQUIRES esp_driver_usb_serial_jtag)\n    endif()\nendif()\n# TESLA_REPAIR_REQUIREMENTS_END\n'
ROOT_ROLE_BLOCK = '# TESLA_BUILD_ROLE_BEGIN\nset(TESLA_BUILD_ROLE "application" CACHE STRING "Closed application or config-repair role")\nif(TESLA_BUILD_ROLE STREQUAL "application")\n    project(tesla-key-esp32)\nelseif(TESLA_BUILD_ROLE STREQUAL "config-repair")\n    project(tesla-key-config-repair)\nelse()\n    message(FATAL_ERROR "Unknown firmware build role")\nendif()\nif(TESLA_BUILD_ROLE STREQUAL "config-repair")\n    string(LENGTH "${PROJECT_VER}" TESLA_REPAIR_VERSION_LENGTH)\n    if(NOT PROJECT_VER MATCHES "^cfgrepair-[0-9a-f]+$" OR NOT TESLA_REPAIR_VERSION_LENGTH EQUAL 26)\n        message(FATAL_ERROR "Maintenance version must be its separate source-fingerprint identity")\n    endif()\n    if(NOT CONFIG_TESLA_CONFIG_REPAIR_USB)\n        message(FATAL_ERROR "Maintenance role requires its explicit configuration")\n    endif()\nelse()\n    if(CONFIG_TESLA_CONFIG_REPAIR_USB)\n        message(FATAL_ERROR "Maintenance configuration cannot build an application artifact")\n    endif()\nendif()\n# TESLA_BUILD_ROLE_END'

SOURCES_BLOCK = '# TESLA_REPAIR_SOURCES_BEGIN\nif(CONFIG_TESLA_CONFIG_REPAIR_USB)\n    target_sources(${COMPONENT_LIB} PRIVATE\n        "config_repair.cpp"\n        "config_repair_console.cpp"\n    )\nendif()\n# TESLA_REPAIR_SOURCES_END\n'
MAINTENANCE_SOURCES = ("config_repair.cpp", "config_repair_console.cpp")

def normal_component_text(root: Path, text: str) -> str:
    if not text.startswith(REQUIREMENTS_PREFIX):
        if "TESLA_REPAIR" in text:
            raise ValueError("maintenance dependency prelude is changed")
        return text  # legacy ordinary synthetic fixture; actual source admission is separate
    if text.count("${TESLA_REPAIR_REQUIRES}") != 1:
        raise ValueError("maintenance dependencies must enter exactly the registered REQUIRES list")
    if (root / "CMakeLists.txt").read_text().count(ROOT_ROLE_BLOCK) != 1:
        raise ValueError("closed project/role/config admission is missing or changed")
    if text.count(SOURCES_BLOCK) != 1:
        raise ValueError("maintenance sources must have exactly the closed enabled-only block")
    for name in MAINTENANCE_SOURCES:
        if text.count('"' + name + '"') != 1:
            raise ValueError("maintenance source escaped its enabled-only block")
    text = text[len(REQUIREMENTS_PREFIX):].replace(SOURCES_BLOCK + "\n", "", 1)
    return text.replace("        ${TESLA_REPAIR_REQUIRES}\n", "", 1)


def component_text(root: Path, text: str, role: str) -> str:
    """The actual closed CMake graph, normalized only for the explicitly admitted role."""
    if role not in ("application", "config-repair"):
        raise ValueError("unknown source inventory role")
    normal = normal_component_text(root, text)
    if role == "application":
        return normal
    if not text.startswith(REQUIREMENTS_PREFIX):
        raise ValueError("maintenance role requires its real enabled-only source block")
    anchor = '        "main.cpp"\n'
    if normal.count(anchor) != 1:
        raise ValueError("maintenance source inventory requires the one actual Main owner")
    return normal.replace(anchor, anchor + ''.join('        "' + n + '"\n' for n in MAINTENANCE_SOURCES), 1)


def configured_project(role: str, sdkconfig: Path, cache: Path) -> str:
    from importlib.util import spec_from_file_location, module_from_spec
    spec=spec_from_file_location("repair_generated_config", Path(__file__).with_name("check-sdkconfig-defaults.py"))
    if spec is None or spec.loader is None: raise ValueError("missing generated config reader")
    reader=module_from_spec(spec);spec.loader.exec_module(reader)
    config=reader.parse_config(sdkconfig)
    expected={"application":APPLICATION_PROJECT,"config-repair":MAINTENANCE_PROJECT}.get(role)
    if expected is None: raise ValueError("unknown build role")
    if (config.get("CONFIG_TESLA_CONFIG_REPAIR_USB")=="y") != (role=="config-repair"):
        raise ValueError("build role/config crossing")
    lines=cache.read_text().splitlines()
    for key,value in (("TESLA_BUILD_ROLE",role),("CMAKE_PROJECT_NAME",expected)):
        rows=[line for line in lines if line.startswith(key+":")]
        if len(rows)!=1 or rows[0].split("=",1)[-1]!=value:
            raise ValueError("configured CMake identity is not the exact role project: "+key)
    return expected
