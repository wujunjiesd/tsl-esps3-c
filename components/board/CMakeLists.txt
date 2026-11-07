idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_io_expander
    PRIV_REQUIRES
        freertos
        esp_io_expander_tca9554
)
