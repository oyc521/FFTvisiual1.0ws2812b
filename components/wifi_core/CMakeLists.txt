idf_component_register(SRCS "wifi_core.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_wifi 
                                  esp_http_server 
                                  nvs_flash 
                                  cjson 
                                  esp_app_format
                                  ota_updater
                                  dual_core_com 
                                  led_controller 
                                  audio_processor)