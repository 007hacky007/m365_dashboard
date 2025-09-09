idf_component_register(SRCS "m365_port.c" "comms_port.c" "protocol_state.c" INCLUDE_DIRS "." REQUIRES driver esp_timer ssd1306)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BUS_TX_INVERT=1)
