idf_component_register(SRCS "dual_core_com.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES freertos led_controller audio_processor)
