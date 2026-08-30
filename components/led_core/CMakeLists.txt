idf_component_register(SRCS "led_core.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES dual_core_com 
                                  led_controller 
                                  led_strip 
                                  audio_processor)
