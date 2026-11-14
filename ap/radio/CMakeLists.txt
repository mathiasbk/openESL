cmake_minimum_required(VERSION 3.16)

# The 802.15.4 radio coprocessor for an AP host on another chip (README.md). It shares the
# link code (esl_coproc), the status screen's text (esl_core) and its panel driver (esl_lcd)
# with the AP.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/esl_coproc"
    "${CMAKE_CURRENT_LIST_DIR}/../components/esl_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/esl_lcd")
set(IDF_TARGET esp32c6)

# The radio board (boards/<board>/, like the AP's): idf.py -DRADIO_BOARD=<board> build, or
# python ../build.py with an AP board that names it. Default: c6, without a screen.
if(NOT DEFINED RADIO_BOARD)
    set(RADIO_BOARD c6)
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${RADIO_BOARD}/radio.json")
    message(FATAL_ERROR "No radio board \"${RADIO_BOARD}\" in ${CMAKE_CURRENT_LIST_DIR}/boards/")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${RADIO_BOARD}/sdkconfig.defaults")
message(STATUS "OpenESL radio board: ${RADIO_BOARD}")

# The AP's version (tools/fw_version.py ap covers ap/, this folder included), so the radio and
# its host from one release say the same version
execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/fw_version.py" ap
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE ver_result)
if(NOT ver_result EQUAL 0)
    message(FATAL_ERROR "tools/fw_version.py ap failed")
endif()

# The project name says which link the image is for, and an AP refuses a radio image for
# another link (esl_coproc_image_kind in esl_coproc.h, #451): "app" in radio.json, esl_radio
# (the radio on UART pins, as every release before) when it has none. The radio takes only an
# image with its own name (radio_main.c), so the two never cross over the air either.
file(READ "${CMAKE_CURRENT_LIST_DIR}/boards/${RADIO_BOARD}/radio.json" radio_json)
string(JSON RADIO_APP ERROR_VARIABLE no_app GET "${radio_json}" app)
if(no_app)
    set(RADIO_APP esl_radio)
endif()
message(STATUS "OpenESL radio app: ${RADIO_APP}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${RADIO_APP})
