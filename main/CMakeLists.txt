idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash dual_core_com wifi_core led_core led_controller audio_processor inmp441_mic utils ota_updater wifi_audio
)
