cmake_minimum_required(VERSION 3.16)
# Ensure all custom components (including u8g2, u8g2-hal-esp-idf, range, etc.) are discoverable.
# Previously only ui and display were added; add the entire components dir so dependencies resolve.
# Dynamically include component dirs that exist (avoids hard failure if a submodule missing in CI)
set(_candidate_component_dirs
	"aht10"
	"display"
	"i2c_v2"
	"i2c_wrap"
	"m365_port"
	"range"
	"settings"
	"ssd1306"
	"u8g2"
	"u8g2-hal-esp-idf"
	"ui"
)
set(EXTRA_COMPONENT_DIRS "")
foreach(_d ${_candidate_component_dirs})
	if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/${_d}")
		list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/${_d}")
	else()
		message(STATUS "Skipping missing component directory: ${_d}")
	endif()
endforeach()
unset(_d)
unset(_candidate_component_dirs)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m365_dash_idf)

# Use custom 4MB OTA partition layout
set(PARTITION_TABLE_FILENAME "partitions_ota.csv")
set(FLASH_SIZE "4MB")
