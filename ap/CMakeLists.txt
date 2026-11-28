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
# The build's variant (boards/README.md, "variants" in board.json): -DVARIANT=<name>, or the older
# -DSNIFFER=1 (the sniffer, docs/radio-sniffer.md) and -DBLE=1 (the Bluetooth AP, docs/ble-tags.md).
# Not kept in the cache: every configure has to ask for it again (build.py does), so a sniffer
# sdkconfig built without it warns below.
set(esl_variant "${VARIANT}")
if(SNIFFER)
    set(esl_variant sniffer)
elseif(BLE)
    set(esl_variant ble)
endif()
unset(VARIANT CACHE)
unset(SNIFFER CACHE)
unset(BLE CACHE)
set(esl_sniffer OFF)
if(esl_variant STREQUAL "sniffer")
    set(esl_sniffer ON)
endif()
if(NOT esl_variant)
    set(esl_variant "-")
endif()
# The sdkconfig.defaults this build reads come from the board's manifest (manifest.py): the common
# ones, the "base" board's, the board's own, the manifest's (the radio driver and the protocols
# that are off, written into the build folder), each protocol's (protocols/*.sdkconfig) and the
# variant's. It also gives the protocols the build has, for GET /api/tags, and the app's name.
execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/manifest.py" cmake "${BOARD}" "${esl_variant}" "${CMAKE_BINARY_DIR}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE manifest_out ERROR_VARIABLE manifest_err RESULT_VARIABLE manifest_result)
if(NOT manifest_result EQUAL 0)
    message(FATAL_ERROR "manifest.py: ${manifest_err}")
endif()
string(REPLACE "\n" ";" manifest_lines "${manifest_out}")
list(GET manifest_lines 0 manifest_files)
string(REPLACE "|" ";" SDKCONFIG_DEFAULTS "${manifest_files}")
list(GET manifest_lines 1 esl_protocols)
if(esl_protocols STREQUAL "-")
    set(esl_protocols "")
endif()
list(GET manifest_lines 2 esl_app)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/manifest.py" "${CMAKE_CURRENT_LIST_DIR}/protocols.json" "${board_json_file}")
message(STATUS "OpenESL AP board ${BOARD}: variant ${esl_variant}, protocols ${esl_protocols}")
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
set(limit_keys runs rects lines icons fileBytes jsonBytes shapes)
set(limit_macros ESL_DRAW_MAX_RUNS ESL_DRAW_MAX_RECTS ESL_DRAW_MAX_LINES ESL_DRAW_MAX_ICONS ESL_LBIN_MAX
    ESL_LAYOUT_MAX_SIZE ESL_DRAW_MAX_SHAPES)
idf_build_set_property(COMPILE_DEFINITIONS "ESL_BOARD=\"${BOARD}\"" APPEND)
string(JSON board_name ERROR_VARIABLE no_name GET "${board_json}" name)
if(no_name OR board_name MATCHES "[\"\\]")
    message(FATAL_ERROR "${board_json_file}: no \"name\", or one with a quote or backslash")
endif()
idf_build_set_property(COMPILE_DEFINITIONS "ESL_BOARD_NAME=\"${board_name}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ESL_PROTOCOLS=\"${esl_protocols}\"" APPEND)
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
# The signal history per tag (ESL_LINK_HIST in esl_store.h): 8 POLLs without PSRAM, where
# every byte of a slot costs tags, 32 where the table is in PSRAM
string(JSON board_psram ERROR_VARIABLE no_psram GET "${board_json}" psram_mb)
if(NOT no_psram AND board_psram GREATER 0)
    idf_build_set_property(COMPILE_DEFINITIONS "ESL_LINK_HIST=32" APPEND)
endif()

# The app's name comes from the manifest ("app" of a variant, esl_ap by default): the Bluetooth AP
# is its own app (esl_ap_ble), as its image goes into its own partition table (ble.partitions.csv,
# no tag firmware partition), so an 802.15.4 AP refuses it over the air and it refuses theirs
# (ota.c, is_ap_image: only an image of this project goes in), and the dashboard and web/build.py
# tell the two apart by the name
project(${esl_app})
# A build folder whose sdkconfig was made for the sniffer stays a sniffer: say so, so it is not
# put on an AP that serves tags by mistake (docs/radio-sniffer.md)
if(CONFIG_ESL_RADIO_SNIFFER AND NOT esl_sniffer)
    message(WARNING "This is a sniffer build (CONFIG_ESL_RADIO_SNIFFER in ${SDKCONFIG}): it answers no tags. "
                    "For the AP, build with python build.py <board>, or give idf.py its own -B folder and -D SDKCONFIG.")
endif()
