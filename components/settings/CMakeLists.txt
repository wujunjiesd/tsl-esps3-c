idf_component_register(
    SRCS "app_settings.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_hw_support
)
