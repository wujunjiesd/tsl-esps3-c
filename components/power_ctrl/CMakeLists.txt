idf_component_register(
    SRCS "power_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        lcd_display
    PRIV_REQUIRES
        esp_driver_gpio
        esp_hw_support
)
