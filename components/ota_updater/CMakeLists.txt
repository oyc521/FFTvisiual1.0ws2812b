idf_component_register(
    SRCS "ota_updater.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update esp_partition esp_system freertos
)
