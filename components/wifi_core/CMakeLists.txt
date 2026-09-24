idf_component_register(SRCS "wifi_core.c"
                    INCLUDE_DIRS "include"
                    EMBED_TXTFILES "assets/loopback.py" "assets/console.html"
                    PRIV_REQUIRES esp_wifi 
                                  esp_http_server 
                                  nvs_flash 
                                  cjson 
                                  esp_app_format
                                  ota_updater
                                  wifi_audio
                                  dual_core_com 
                                  led_controller 
                                  audio_processor)