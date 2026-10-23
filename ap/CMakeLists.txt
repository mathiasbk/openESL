cmake_minimum_required(VERSION 3.16)

# The version is the release the AP sources last changed in (tools/fw_version.py, from the VERSION
# file and the release tags), so a release that changes only the tags gives the AP nothing new.
# esp_app_get_description()->version reports it, and GET /api/tags returns it as ap.fw.
execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../tools/fw_version.py" ap
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE ver_result)
if(NOT ver_result EQUAL 0)
    message(FATAL_ERROR "tools/fw_version.py ap failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")

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
# first build). Commit it like the C6's: the C5 and S3 lock files are not in the repo yet,
# so those builds take the newest components that match ap/main/idf_component.yml.
if(NOT board_target STREQUAL "esp32c6")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${board_target}")
endif()

# The board's layout limits ("limits" in board.json, boards/README.md) for the layout compiler
# and renderer in esl_core; a limit the board does not set keeps the default in shared/proto.h
# or esl_layout.h (the ESP32-C6's). web/build.py reads the same keys for the web renderer.
set(limit_keys runs rects lines icons fileBytes jsonBytes)
set(limit_macros ESL_DRAW_MAX_RUNS ESL_DRAW_MAX_RECTS ESL_DRAW_MAX_LINES ESL_DRAW_MAX_ICONS ESL_LBIN_MAX
    ESL_LAYOUT_MAX_SIZE)
idf_build_set_property(COMPILE_DEFINITIONS "ESL_BOARD=\"${BOARD}\"" APPEND)
string(JSON board_limits ERROR_VARIABLE no_limits GET "${board_json}" limits)
if(NOT no_limits)
    string(JSON n_limits LENGTH "${board_limits}")
endif()
if(NOT no_limits AND n_limits GREATER 0)
    math(EXPR last_limit "${n_limits} - 1")
    foreach(i RANGE ${last_limit})
        string(JSON key MEMBER "${board_limits}" ${i})
        string(JSON value GET "${board_limits}" ${key})
        list(FIND limit_keys "${key}" at)
        if(at LESS 0 OR NOT value MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "${board_json_file}: limits.${key} = ${value} is not a known limit with a whole number")
        endif()
        list(GET limit_macros ${at} macro)
        idf_build_set_property(COMPILE_DEFINITIONS "${macro}=${value}" APPEND)
        message(STATUS "OpenESL AP board ${BOARD}: ${key} = ${value}")
    endforeach()
endif()

project(esl_ap)
