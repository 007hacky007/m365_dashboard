cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/ui" "${CMAKE_CURRENT_LIST_DIR}/components/display")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m365_dash_idf)

# Use custom 4MB OTA partition layout
set(PARTITION_TABLE_FILENAME "partitions_ota.csv")
set(FLASH_SIZE "4MB")
