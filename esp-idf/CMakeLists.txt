cmake_minimum_required(VERSION 3.16)
# Ensure all custom components (including u8g2, u8g2-hal-esp-idf, range, etc.) are discoverable.
# Previously only ui and display were added; add the entire components dir so dependencies resolve.
# Explicit component directory list (order can matter for some legacy dependencies)
set(EXTRA_COMPONENT_DIRS
	"${CMAKE_CURRENT_LIST_DIR}/components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m365_dash_idf)

# Use custom 4MB OTA partition layout
set(PARTITION_TABLE_FILENAME "partitions_ota.csv")
set(FLASH_SIZE "4MB")
