idf_component_register(SRCS "hello_world_main.c" "i2c_lcd.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_adc esp_timer)
