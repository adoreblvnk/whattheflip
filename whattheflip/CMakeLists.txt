cmake_minimum_required(VERSION 3.16)

# Include mtkernel and freertos_shim components from third_party/esp32c6-mtk3
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../third_party/esp32c6-mtk3/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whattheflip)

