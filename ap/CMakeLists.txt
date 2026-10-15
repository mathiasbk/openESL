cmake_minimum_required(VERSION 3.16)

# The version is in the VERSION file at the top of the repository (shared with the tag firmware).
# esp_app_get_description()->version reports it, and GET /api/tags returns it as ap.fw.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esl_ap)
