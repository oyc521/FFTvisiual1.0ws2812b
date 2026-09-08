#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_updater_register_httpd(httpd_handle_t server);

esp_err_t ota_updater_confirm_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATER_H */
