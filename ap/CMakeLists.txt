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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esl_ap)
