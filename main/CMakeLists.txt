idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        settings
        board
        lcd_display
        power_ctrl
        boot_button
        wifi_portal
        tesla_ble
        audio_prompt
    PRIV_REQUIRES
        nvs_flash
        esp_system
)
