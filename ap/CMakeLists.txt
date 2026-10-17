cmake_minimum_required(VERSION 3.16)

# The version is in the VERSION file at the top of the repository (shared with the tag firmware).
# esp_app_get_description()->version reports it, and GET /api/tags returns it as ap.fw.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)

# The board (boards/<board>/, see boards/README.md): idf.py -DBOARD=<board> build, or
# python build.py <board>. The default is the ESP32-C6 Super Mini, so a plain
# "idf.py build" builds what it always did. The board's sdkconfig.defaults (after its
# "base" board's, if it has one) is read after the common sdkconfig.defaults.
if(NOT DEFINED BOARD)
    set(BOARD c6-supermini)
endif()
set(board_json_file "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/board.json")
if(NOT EXISTS "${board_json_file}")
    message(FATAL_ERROR "No AP board \"${BOARD}\": there is no ${board_json_file}")
endif()
file(READ "${board_json_file}" board_json)
string(JSON board_target GET "${board_json}" target)
string(JSON board_base ERROR_VARIABLE no_base GET "${board_json}" base)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(NOT no_base)
    list(APPEND SDKCONFIG_DEFAULTS "boards/${board_base}/sdkconfig.defaults")
endif()
list(APPEND SDKCONFIG_DEFAULTS "boards/${BOARD}/sdkconfig.defaults")
# The chip comes from the board, not from a sdkconfig left in this folder by another build
if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "${board_target}")
    message(FATAL_ERROR "IDF_TARGET is $ENV{IDF_TARGET}, but board ${BOARD} is ${board_target}")
endif()
set(IDF_TARGET "${board_target}")
message(STATUS "OpenESL AP board: ${BOARD} (${board_target})")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# dependencies.lock pins the managed components for the ESP32-C6 (requirement G5); other
# chips get their own lock file next to it (the component manager writes it on the
# first build, and it is committed like the C6's).
if(NOT board_target STREQUAL "esp32c6")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${board_target}")
endif()

project(esl_ap)
