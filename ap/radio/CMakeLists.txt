cmake_minimum_required(VERSION 3.16)

# The 802.15.4 radio coprocessor for an AP host on another chip (README.md). It shares
# only the link code with the AP (../components/esl_link).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/esl_link")
set(IDF_TARGET esp32c6)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esl_radio)
