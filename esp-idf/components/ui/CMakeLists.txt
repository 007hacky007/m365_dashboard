idf_component_register(SRCS "display_task.c"
                       INCLUDE_DIRS "." "../m365_port" "../ssd1306"
                       REQUIRES m365_port ssd1306 esp_timer)
