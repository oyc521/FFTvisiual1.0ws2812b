idf_component_register(SRCS "led_controller.c"
                    INCLUDE_DIRS "include"
    REQUIRES                          # 依赖的组件
        driver
        esp_driver_rmt
        led_strip
        utils
    PRIV_REQUIRES freertos
)