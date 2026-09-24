# components/audio_processor/CMakeLists.txt
idf_component_register(
    SRCS "audio_processor.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        inmp441_mic
        utils
        wifi_audio
    PRIV_REQUIRES 
        esp-dsp
        driver
        esp_timer
        freertos
)