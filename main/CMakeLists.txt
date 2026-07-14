idf_component_register(SRCS "main.c" "tps61165.c" "stusb4500.c" "stusb4500_nvm.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer)
