idf_component_register(
    SRCS "wifi_audio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip freertos esp_timer esp_netif
)
