cmake_minimum_required(VERSION 3.16)

# The 802.15.4 radio coprocessor for an AP host on another chip (README.md). It shares
# only the link code with the AP (../components/esl_coproc).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/esl_coproc")
set(IDF_TARGET esp32c6)

# The AP's version (tools/fw_version.py ap covers ap/, this folder included), so the radio and
# its host from one release say the same version
execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/fw_version.py" ap
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE ver_result)
if(NOT ver_result EQUAL 0)
    message(FATAL_ERROR "tools/fw_version.py ap failed")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esl_radio)
