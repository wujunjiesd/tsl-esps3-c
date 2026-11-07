idf_component_register(
    SRCS "wifi_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES
        settings
        tesla_ble
    PRIV_REQUIRES
        esp_wifi
        esp_http_server
        esp_netif
        esp_event
        lwip
        esp_system
    EMBED_FILES "assets/index.html"
)
