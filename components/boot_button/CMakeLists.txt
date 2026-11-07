idf_component_register(
    SRCS "boot_button.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lcd_display
        settings
    PRIV_REQUIRES
        esp_driver_gpio
        esp_system
)
