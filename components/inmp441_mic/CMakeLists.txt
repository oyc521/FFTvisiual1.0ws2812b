# components/inmp441_mic/CMakeLists.txt
idf_component_register(
    SRCS "inmp441_mic.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
             esp_driver_gpio
             esp_driver_i2s
    PRIV_REQUIRES esp_timer
    freertos
)