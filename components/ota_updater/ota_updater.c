#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "ota_updater.h"

static const char *TAG = "OTA_UPDATER";

static esp_err_t cors_preflight_handler(httpd_req_t *req);

#define OTA_RECV_BUF_SIZE 4096

static bool s_ota_busy = false;

static void ota_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
    vTaskDelete(NULL);
}

static void schedule_reboot(void)
{
    xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Accept");
    if (s_ota_busy) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "OTA already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    s_ota_busy = true;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(running);
    if (update == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "No OTA partition", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA target partition: %s @ 0x%x size 0x%x",
             update->label, update->address, update->size);

    if (req->content_len > (int)update->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Firmware larger than OTA partition", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t ret = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "esp_ota_begin failed", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    char *buf = malloc(OTA_RECV_BUF_SIZE);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Out of memory", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    size_t written = 0;
    int received;
    bool fail = false;
    esp_err_t ota_err = ESP_OK;

    while (1) {
        received = httpd_req_recv(req, buf, OTA_RECV_BUF_SIZE);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (received == 0) {
                ESP_LOGW(TAG, "Connection closed by peer before data finished");
            } else {
                ESP_LOGW(TAG, "Socket error during OTA receive: %d", received);
            }
            fail = true;
            break;
        }
        ota_err = esp_ota_write(ota_handle, buf, received);
        if (ota_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ota_err));
            fail = true;
            break;
        }
        written += received;
        if (req->content_len > 0 && written >= (size_t)req->content_len) {
            break;
        }
    }

    free(buf);

    if (fail) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "OTA upload interrupted", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    if (req->content_len > 0 && written != (size_t)req->content_len) {
        ESP_LOGE(TAG, "Received %d bytes, expected %d bytes",
                 (int)written, (int)req->content_len);
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Size mismatch", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed, image invalid: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Firmware image invalid", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    ret = esp_ota_set_boot_partition(update);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Set boot partition failed", HTTPD_RESP_USE_STRLEN);
        s_ota_busy = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA success, %d bytes written to %s, rebooting...",
             (int)written, update->label);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    schedule_reboot();
    return ESP_OK;
}

static const httpd_uri_t ota_upload_uri = {
    .uri       = "/api/ota",
    .method    = HTTP_POST,
    .handler   = ota_upload_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ota_preflight_uri = {
    .uri       = "/api/ota",
    .method    = HTTP_OPTIONS,
    .handler   = cors_preflight_handler,
    .user_ctx  = NULL
};

static esp_err_t cors_preflight_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Accept");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t ota_updater_register_httpd(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = httpd_register_uri_handler(server, &ota_upload_uri);
    if (ret != ESP_OK) {
        return ret;
    }
    return httpd_register_uri_handler(server, &ota_preflight_uri);
}

esp_err_t ota_updater_confirm_boot(void)
{
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Boot image confirmed valid, rollback cancelled");
    } else {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(ret));
    }
    return ret;
}
