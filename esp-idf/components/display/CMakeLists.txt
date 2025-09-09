idf_component_register(SRCS "u8g2_integration.c" "display_fsm_port.c"
                       INCLUDE_DIRS "." "../m365_port" "../../../M365/fonts"
                       REQUIRES driver m365_port esp_timer u8g2-hal-esp-idf range settings aht10
                       PRIV_REQUIRES main)

# NOTE: Removed legacy u8g2_port.c (custom minimal framebuffer) now that official
# u8g2 is integrated. Also dropped private dependency on i2c_v2 to avoid
# double-initializing I2C when using the HAL implementation.
