#include "wifi_core.h"
#include "dual_core_com.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip4_addr.h"
#include "cJSON.h"
#include "ota_updater.h"
#include <string.h>
#include <stdlib.h>

// For captive portal DNS server
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "WIFI_CORE";

// WiFi事件组位定义
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_AP_STARTED_BIT BIT2

// NVS配置键值
#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_DEVICE_NAME "device_name"
#define NVS_KEY_WIFI_CONF_SAVED "wifi_conf_saved"

// 全局变量
static TaskHandle_t wifi_core_task_handle = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t server = NULL;
static wifi_core_config_t s_wifi_config;
static web_core_config_t s_web_config;
static bool s_is_ap_mode = false;
static bool s_wifi_configured = false;
static char s_stored_ssid[33] = {0};
static char s_stored_password[65] = {0};
static char s_device_name[33] = {0};

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Accept");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t cors_preflight_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// 内部函数声明
static esp_err_t start_ap_mode(void);
static esp_err_t start_sta_mode(void);
static esp_err_t save_wifi_config_to_nvs(const char* ssid, const char* password);
static esp_err_t load_wifi_config_from_nvs(void);
static esp_err_t start_web_server(void);
static esp_err_t stop_web_server(void);
static esp_err_t wifi_core_start_api_server(void);

// Captive portal / DNS helpers
static void dns_server_task(void *pvParameters);
static esp_err_t start_dns_server(void);
static esp_err_t stop_dns_server(void);
static esp_err_t ap_captive_redirect_handler(httpd_req_t *req);

static TaskHandle_t dns_task_handle = NULL;
static int dns_sock = -1;

// WiFi事件处理函数
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                dual_core_com_set_wifi_status(true);
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                    ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
                    dual_core_com_set_wifi_status(false);
                    
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                    
                    // 如果已配置WiFi，尝试重连
                    if (!s_is_ap_mode && s_wifi_configured) {
                        ESP_LOGI(TAG, "Attempting to reconnect in 5 seconds...");
                        vTaskDelay(5000 / portTICK_PERIOD_MS);
                        esp_wifi_connect();
                    } else if (!s_wifi_configured) {
                        // 未配置WiFi，保持AP模式
                        ESP_LOGI(TAG, "WiFi not configured, staying in AP mode");
                    }
                }
                break;
                
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "WiFi AP started");
                s_is_ap_mode = true;
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_AP_STARTED_BIT);
                }
                break;
                
            case WIFI_EVENT_AP_STACONNECTED:
                {
                    wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                    const uint8_t *mac = event->mac;
                    ESP_LOGI(TAG, "Device %02x:%02x:%02x:%02x:%02x:%02x connected to hotspot",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                }
                break; 
                
            case WIFI_EVENT_AP_STADISCONNECTED:
                {
                    wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                    const uint8_t *mac = event->mac;
                    ESP_LOGI(TAG, "Device %02x:%02x:%02x:%02x:%02x:%02x disconnected",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                }
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP:
                {
                    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
                    
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                    }
                    
                    // 启动API服务器
                    wifi_core_start_api_server();
                }
                break;
                
            case IP_EVENT_AP_STAIPASSIGNED:
                {
                    // AP模式有设备连接
                    ESP_LOGI(TAG, "Device connected to AP, IP assigned");
                    // 启动配网Web服务器
                    start_web_server();
                }
                break;
                
            default:
                break;
        }
    }
}

// ========================== AP模式处理函数 ==========================

// 处理AP模式的根路径请求（极简API说明页面）
static esp_err_t ap_root_get_handler(httpd_req_t *req)
{
    int sock = httpd_req_to_sockfd(req);
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char client_ip[32] = "<unknown>";
    if (getpeername(sock, (struct sockaddr*)&peer, &peer_len) == 0) {
        const char* tmp = inet_ntoa(peer.sin_addr);
        if (tmp) strncpy(client_ip, tmp, sizeof(client_ip)-1);
    }
    ESP_LOGI(TAG, "HTTP GET / from %s (AP mode)", client_ip);

    const char *page = 
        "<!DOCTYPE html>"
        "<html><body>"
        "<h1>LED Controller - Provisioning</h1>"
        "<p>Device: %s</p>"
        "<p>Use the diagnostic HTML file to control the device.</p>"
        "<p>Available APIs:</p>"
        "<ul>"
        "<li>GET /ping - Connectivity test</li>"
        "<li>GET /api/status - Get device status</li>"
        "<li>POST /api/provision - Configure WiFi (with JSON {ssid, password})</li>"
        "</ul>"
        "</body></html>";
    
    char response[512];
    snprintf(response, sizeof(response), page, s_device_name);
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, response, strlen(response));
} 

// API: 配网接口
static void provision_apply_task(void *pvParameters)
{
    // give client time to receive response
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Provision apply task: stopping AP web server and switching to STA mode");
    // Stop AP web server and DNS
    stop_web_server();

    if (start_sta_mode() != ESP_OK) {
        ESP_LOGW(TAG, "Provision apply: start_sta_mode failed, rebooting to apply settings");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    vTaskDelete(NULL);
}

static esp_err_t api_provision_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddBoolToObject(error_response, "success", false);
        cJSON_AddStringToObject(error_response, "error", "Failed to receive data");
        
        char *response_str = cJSON_PrintUnformatted(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        
        free(response_str);
        cJSON_Delete(error_response);
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddBoolToObject(error_response, "success", false);
        cJSON_AddStringToObject(error_response, "error", "Invalid JSON");
        
        char *response_str = cJSON_PrintUnformatted(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        
        free(response_str);
        cJSON_Delete(error_response);
        return ESP_FAIL;
    }
    
    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(json, "password");
    
    if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring) {
        cJSON_Delete(json);
        
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddBoolToObject(error_response, "success", false);
        cJSON_AddStringToObject(error_response, "error", "SSID is required");
        
        char *response_str = cJSON_PrintUnformatted(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        
        free(response_str);
        cJSON_Delete(error_response);
        return ESP_FAIL;
    }
    
    const char *ssid = ssid_item->valuestring;
    const char *password = password_item ? password_item->valuestring : "";
    
    // 保存配置到NVS
    esp_err_t save_ret = save_wifi_config_to_nvs(ssid, password);
    
    cJSON *response = cJSON_CreateObject();
    if (save_ret == ESP_OK) {
        cJSON_AddBoolToObject(response, "success", true);
        cJSON_AddStringToObject(response, "message", "WiFi configuration saved successfully");
        
        // 更新全局变量
        s_wifi_configured = true;
        strncpy(s_stored_ssid, ssid, sizeof(s_stored_ssid) - 1);
        strncpy(s_stored_password, password, sizeof(s_stored_password) - 1);
        
        ESP_LOGI(TAG, "WiFi configuration saved: SSID=%s", ssid);

        // Apply configuration asynchronously to avoid blocking HTTP handler
        if (xTaskCreate(provision_apply_task, "provision_apply", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create provisioning apply task, rebooting to apply configuration");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    } else {
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "error", "Failed to save WiFi configuration");
    }
    
    char *response_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    
    free(response_str);
    cJSON_Delete(json);
    cJSON_Delete(response);
    
    return ESP_OK;
}

// ========================== STA模式API处理函数 ==========================

// API: 获取当前模式
static esp_err_t api_mode_get_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    core_status_t status;
    dual_core_com_get_status(&status);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mode", status.current_mode);
    cJSON_AddStringToObject(root, "device_name", s_device_name);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_mode_set_handler(httpd_req_t *req) {
    add_cors_headers(req);
    ESP_LOGI(TAG, "Receive mode set request");
    
    char query_str[32] = {0};
    esp_err_t query_ret = httpd_req_get_url_query_str(req, query_str, sizeof(query_str));
    if (query_ret != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "error", "Failed to get query string");
        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    char mode_str[8] = {0};
    if (httpd_query_key_value(query_str, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
        ESP_LOGI(TAG, "Mode string: '%s'", mode_str);
        
        if (strlen(mode_str) == 0) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", false);
            cJSON_AddStringToObject(root, "error", "Mode parameter is empty");
            char *json_str = cJSON_PrintUnformatted(root);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, strlen(json_str));
            free(json_str);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        
        int mode = atoi(mode_str);
        ESP_LOGI(TAG, "Mode value: %d", mode);

        // 检查模式值是否有效 (0-12)
        if (mode < MODE_SPECTRUM || mode > MODE_OFF) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", false);
            cJSON_AddStringToObject(root, "error", "Mode value out of range");
            cJSON_AddNumberToObject(root, "valid_min", 0);
            cJSON_AddNumberToObject(root, "valid_max", 12);
            char *json_str = cJSON_PrintUnformatted(root);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, strlen(json_str));
            free(json_str);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        
        // 创建命令
        core_command_t cmd = {
            .type = CMD_MODE_CHANGE,
            .data.mode = (led_mode_t)mode
        };
        
        ESP_LOGI(TAG, "Sending mode change command: %d", mode);
        
        // 发送命令
        esp_err_t send_result = dual_core_com_send_command(&cmd, pdMS_TO_TICKS(100));
        
        if (send_result == ESP_OK) {
            core_status_t status;
            dual_core_com_get_status(&status);
            
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON_AddNumberToObject(root, "mode", mode);
            cJSON_AddNumberToObject(root, "current_mode", status.current_mode);
            cJSON_AddStringToObject(root, "message", "Mode change command sent successfully");
            
            char *json_str = cJSON_PrintUnformatted(root);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, strlen(json_str));
            
            free(json_str);
            cJSON_Delete(root);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to send command");
        }
    } else {
        ESP_LOGE(TAG, "Failed to parse mode parameter");
    }
    
    // 错误处理
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", "Failed to parse mode parameter");
    cJSON_AddStringToObject(root, "query_string", query_str);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_FAIL;
}

// API: 发送命令
static esp_err_t api_command_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    char query_str[32] = {0};
    if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) == ESP_OK) {
        char cmd_str[32] = {0};
        if (httpd_query_key_value(query_str, "cmd", cmd_str, sizeof(cmd_str)) == ESP_OK) {
            core_command_t cmd;
            
            if (strcmp(cmd_str, "brightness_up") == 0) {
                cmd.type = CMD_BRIGHTNESS_UP;
            } else if (strcmp(cmd_str, "brightness_down") == 0) {
                cmd.type = CMD_BRIGHTNESS_DOWN;
            } else if (strcmp(cmd_str, "test_rainbow") == 0) {
                cmd.type = CMD_TEST_RAINBOW;
            } else if (strcmp(cmd_str, "clear_all") == 0) {
                cmd.type = CMD_MODE_CHANGE;
                cmd.data.mode = MODE_OFF;
            } else {
                cJSON *root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "success", false);
                cJSON_AddStringToObject(root, "error", "Unknown command");
                char *json_str = cJSON_PrintUnformatted(root);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, json_str, strlen(json_str));
                free(json_str);
                cJSON_Delete(root);
                return ESP_FAIL;
            }
            
            esp_err_t send_result = dual_core_com_send_command(&cmd, pdMS_TO_TICKS(100));
            
            cJSON *root = cJSON_CreateObject();
            if (send_result == ESP_OK) {
                cJSON_AddBoolToObject(root, "success", true);
                cJSON_AddStringToObject(root, "command", cmd_str);
                cJSON_AddStringToObject(root, "message", "Command sent successfully");
            } else {
                cJSON_AddBoolToObject(root, "success", false);
                cJSON_AddStringToObject(root, "command", cmd_str);
                cJSON_AddStringToObject(root, "error", "Failed to send command");
            }
            
            char *json_str = cJSON_PrintUnformatted(root);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, strlen(json_str));
            
            free(json_str);
            cJSON_Delete(root);
            return ESP_OK;
        }
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", "Invalid command format");
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_FAIL;
}

// API: 获取状态
static esp_err_t api_status_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    core_status_t status;
    dual_core_com_get_status(&status);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mode", status.current_mode);
    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    cJSON_AddNumberToObject(root, "frame_count", status.led_frame_count);
    cJSON_AddNumberToObject(root, "free_heap", status.free_heap_size);
    cJSON_AddBoolToObject(root, "is_ap_mode", s_is_ap_mode);
    cJSON_AddBoolToObject(root, "wifi_configured", s_wifi_configured);
    cJSON_AddNumberToObject(root, "brightness", status.brightness);
    cJSON_AddNumberToObject(root, "bpm", status.bpm);
    cJSON_AddNumberToObject(root, "pulse", status.pulse);
    cJSON_AddNumberToObject(root, "energy", status.energy);
    cJSON_AddNumberToObject(root, "gamma", status.gamma);
    cJSON_AddNumberToObject(root, "gate", status.gate);
    cJSON_AddNumberToObject(root, "afterimage", status.afterimage);
    cJSON_AddStringToObject(root, "device_name", s_device_name);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc != NULL) {
        cJSON_AddStringToObject(root, "fw_version", app_desc->version);
        cJSON_AddStringToObject(root, "project_name", app_desc->project_name);
        cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    }
    
    // 获取IP地址
    char ip_buffer[16] = {0};
    if (wifi_core_get_ip_address(ip_buffer, sizeof(ip_buffer)) == ESP_OK) {
        cJSON_AddStringToObject(root, "ip_address", ip_buffer);
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// /ping handler for connectivity test
static esp_err_t ping_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "pong", 4);
}

static esp_err_t api_brightness_get_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    core_status_t status;
    dual_core_com_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "brightness", status.brightness);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_brightness_set_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    char query_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"no value\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char value_str[8] = {0};
    if (httpd_query_key_value(query_str, "value", value_str, sizeof(value_str)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"missing value\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int value = atoi(value_str);
    if (value < 0 || value > 100) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"out of range 0-100\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    core_command_t cmd = {
        .type = CMD_BRIGHTNESS_SET,
    };
    cmd.data.param.value = value;

    esp_err_t ret = dual_core_com_send_command(&cmd, pdMS_TO_TICKS(100));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", ret == ESP_OK);
    cJSON_AddNumberToObject(root, "brightness", value);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static float qs_get_float(httpd_req_t *req, const char *key, float def)
{
    char q[80] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return def;
    char val[16] = {0};
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) return def;
    return strtof(val, NULL);
}

static esp_err_t api_post_get_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    core_status_t status;
    dual_core_com_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "gamma", status.gamma);
    cJSON_AddNumberToObject(root, "gate", status.gate);
    cJSON_AddNumberToObject(root, "afterimage", status.afterimage);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_post_set_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    float gamma = qs_get_float(req, "gamma", 1.0f);
    int gate = (int)qs_get_float(req, "gate", 0.0f);
    float afterimage = qs_get_float(req, "afterimage", 0.0f);
    if (gate < 0) gate = 0;
    if (gate > 64) gate = 64;

    core_command_t cmd = { .type = CMD_POST_SET };
    cmd.data.post.gamma = gamma;
    cmd.data.post.gate = (uint8_t)gate;
    cmd.data.post.afterimage = afterimage;

    esp_err_t ret = dual_core_com_send_command(&cmd, pdMS_TO_TICKS(100));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", ret == ESP_OK);
    cJSON_AddNumberToObject(root, "gamma", gamma);
    cJSON_AddNumberToObject(root, "gate", gate);
    cJSON_AddNumberToObject(root, "afterimage", afterimage);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// STA模式根路径处理器（极简页面）
static esp_err_t sta_root_handler(httpd_req_t *req)
{
    char ip_buffer[16] = "192.168.4.1";
    wifi_core_get_ip_address(ip_buffer, sizeof(ip_buffer));
    
    const char *page = 
        "<!DOCTYPE html>"
        "<html><head><meta charset=\"UTF-8\"><title>LED Controller API</title></head>"
        "<body>"
        "<h1>LED Controller - API Server</h1>"
        "<p>Device: %s</p>"
        "<p>IP Address: %s</p>"
        "<p>Use the led_diagnostic.html file to control this device.</p>"
        "<p>Available APIs:</p>"
        "<ul>"
        "<li>GET /ping - Connectivity test</li>"
        "<li>GET /api/status - Get device status</li>"
        "<li>GET /api/mode - Get current mode</li>"
        "<li>POST /api/mode?mode=X - Set mode (0-8)</li>"
        "<li>POST /api/command?cmd=XXX - Send command</li>"
        "</ul>"
        "</body></html>";
    
    char response[1024];
    snprintf(response, sizeof(response), page, s_device_name, ip_buffer);
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, response, strlen(response));
}

// ==================== URI处理器定义 ====================

static const httpd_uri_t ap_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = ap_root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t sta_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = sta_root_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_provision = {
    .uri       = "/api/provision",
    .method    = HTTP_POST,
    .handler   = api_provision_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t captive_generate_204 = {
    .uri       = "/generate_204",
    .method    = HTTP_GET,
    .handler   = ap_captive_redirect_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t captive_hotspot_detect = {
    .uri       = "/hotspot-detect.html",
    .method    = HTTP_GET,
    .handler   = ap_captive_redirect_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t captive_ncsi = {
    .uri       = "/ncsi.txt",
    .method    = HTTP_GET,
    .handler   = ap_captive_redirect_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_ping = {
    .uri       = "/ping",
    .method    = HTTP_GET,
    .handler   = ping_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_mode_get = {
    .uri       = "/api/mode",
    .method    = HTTP_GET,
    .handler   = api_mode_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_mode_set = {
    .uri       = "/api/mode",
    .method    = HTTP_POST,
    .handler   = api_mode_set_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_command = {
    .uri       = "/api/command",
    .method    = HTTP_POST,
    .handler   = api_command_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_status = {
    .uri       = "/api/status",
    .method    = HTTP_GET,
    .handler   = api_status_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_brightness_get = {
    .uri       = "/api/brightness",
    .method    = HTTP_GET,
    .handler   = api_brightness_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_brightness_set = {
    .uri       = "/api/brightness",
    .method    = HTTP_POST,
    .handler   = api_brightness_set_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_post_get = {
    .uri       = "/api/post",
    .method    = HTTP_GET,
    .handler   = api_post_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t api_post_set = {
    .uri       = "/api/post",
    .method    = HTTP_POST,
    .handler   = api_post_set_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t preflight_post = {
    .uri       = "/api/post",
    .method    = HTTP_OPTIONS,
    .handler   = cors_preflight_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t preflight_mode = {
    .uri       = "/api/mode",
    .method    = HTTP_OPTIONS,
    .handler   = cors_preflight_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t preflight_command = {
    .uri       = "/api/command",
    .method    = HTTP_OPTIONS,
    .handler   = cors_preflight_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t preflight_provision = {
    .uri       = "/api/provision",
    .method    = HTTP_OPTIONS,
    .handler   = cors_preflight_handler,
    .user_ctx  = NULL
};

// 启动AP模式的Web服务器
static esp_err_t start_web_server(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 20;
    config.core_id = 0;
    
    ESP_LOGI(TAG, "Starting AP mode web server on port %d", config.server_port);
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP web server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册AP模式的URI处理器
    httpd_register_uri_handler(server, &ap_root);
    httpd_register_uri_handler(server, &api_provision);
    httpd_register_uri_handler(server, &api_status);
    httpd_register_uri_handler(server, &api_ping);
    httpd_register_uri_handler(server, &api_brightness_get);
    httpd_register_uri_handler(server, &api_brightness_set);
    httpd_register_uri_handler(server, &api_post_get);
    httpd_register_uri_handler(server, &api_post_set);
    httpd_register_uri_handler(server, &preflight_post);
    httpd_register_uri_handler(server, &captive_generate_204);
    httpd_register_uri_handler(server, &captive_hotspot_detect);
    httpd_register_uri_handler(server, &captive_ncsi);
    httpd_register_uri_handler(server, &preflight_provision);
    ota_updater_register_httpd(server);

    // 启动 DNS 劫持服务
    if (start_dns_server() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start captive DNS server");
    }

    ESP_LOGI(TAG, "AP mode web server started successfully");
    return ESP_OK;
}

// 启动STA模式的API服务器
static esp_err_t wifi_core_start_api_server(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = s_web_config.port;
    config.max_uri_handlers = 20;
    config.core_id = 0;
    
    ESP_LOGI(TAG, "Starting STA API server on port %d", config.server_port);
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start API server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册STA模式的URI处理器
    httpd_register_uri_handler(server, &sta_root);
    httpd_register_uri_handler(server, &api_mode_get);
    httpd_register_uri_handler(server, &api_mode_set);
    httpd_register_uri_handler(server, &api_command);
    httpd_register_uri_handler(server, &api_status);
    httpd_register_uri_handler(server, &api_ping);
    httpd_register_uri_handler(server, &api_brightness_get);
    httpd_register_uri_handler(server, &api_brightness_set);
    httpd_register_uri_handler(server, &api_post_get);
    httpd_register_uri_handler(server, &api_post_set);
    httpd_register_uri_handler(server, &preflight_post);
    httpd_register_uri_handler(server, &preflight_mode);
    httpd_register_uri_handler(server, &preflight_command);
    ota_updater_register_httpd(server);

    ESP_LOGI(TAG, "STA API server started successfully");
    return ESP_OK;
}

// Captive portal redirect handler
static esp_err_t ap_captive_redirect_handler(httpd_req_t *req)
{
    const char *redirect_url = "http://192.168.4.1/";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// DNS 劫持后台任务（极简实现：对任何查询返回 192.168.4.1）
static void dns_server_task(void *pvParameters)
{
    struct sockaddr_in addr;
    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(dns_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed: %d", errno);
        close(dns_sock);
        dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS server started on port 53");

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int lenr = recvfrom(dns_sock, buf, sizeof(buf), 0, (struct sockaddr*)&client, &client_len);
        if (lenr <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (lenr < 12) continue;
        int idx = 12;
        
        // 跳过查询名称
        while (idx < lenr && buf[idx] != 0) {
            idx += 1 + buf[idx];
        }
        if (idx + 5 > lenr) continue;
        int qlen = idx + 5;

        // 构造DNS响应
        uint8_t resp[512];
        memset(resp, 0, sizeof(resp));
        resp[0] = buf[0]; resp[1] = buf[1];  // Transaction ID
        resp[2] = 0x81; resp[3] = 0x80;      // Flags
        resp[4] = buf[4]; resp[5] = buf[5];  // Questions
        resp[6] = 0x00; resp[7] = 0x01;      // Answers = 1
        resp[8] = 0x00; resp[9] = 0x00;      // NS, AR = 0
        resp[10] = 0x00; resp[11] = 0x00;
        
        memcpy(&resp[12], &buf[12], qlen);
        int off = 12 + qlen;
        
        resp[off++] = 0xc0; resp[off++] = 0x0c;  // Name pointer
        resp[off++] = 0x00; resp[off++] = 0x01;  // TYPE A
        resp[off++] = 0x00; resp[off++] = 0x01;  // CLASS IN
        resp[off++] = 0x00; resp[off++] = 0x00;  // TTL (300s)
        resp[off++] = 0x01; resp[off++] = 0x2c;
        resp[off++] = 0x00; resp[off++] = 0x04;  // RDLENGTH 4
        resp[off++] = 192; resp[off++] = 168;    // RDATA = 192.168.4.1
        resp[off++] = 4; resp[off++] = 1;

        int resp_len = off;
        sendto(dns_sock, resp, resp_len, 0, (struct sockaddr*)&client, client_len);
    }

    if (dns_sock >= 0) {
        close(dns_sock);
        dns_sock = -1;
    }
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void)
{
    if (dns_task_handle != NULL) return ESP_OK;
    BaseType_t res = xTaskCreatePinnedToCore(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle, 0);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create dns server task");
        dns_task_handle = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stop_dns_server(void)
{
    if (dns_task_handle) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
    if (dns_sock >= 0) {
        close(dns_sock);
        dns_sock = -1;
    }
    return ESP_OK;
}

// 停止Web服务器
static esp_err_t stop_web_server(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }

    stop_dns_server();

    return ESP_OK;
}

// 启动AP模式
static esp_err_t start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode...");
    
    if (!s_is_ap_mode) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    
    s_ap_netif = esp_netif_create_default_wifi_ap();
    start_web_server();
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(s_device_name),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    
    strncpy((char*)ap_config.ap.ssid, s_device_name, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, "12345678", sizeof(ap_config.ap.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);
    
    s_is_ap_mode = true;
    ESP_LOGI(TAG, "AP mode started - SSID: %s, Password: 12345678", s_device_name);
    
    return ESP_OK;
}

// 启动STA模式
static esp_err_t start_sta_mode(void)
{
    ESP_LOGI(TAG, "Starting STA mode...");

    stop_dns_server();
    
    if (strlen(s_stored_ssid) == 0) {
        ESP_LOGE(TAG, "No WiFi configuration found");
        return ESP_FAIL;
    }
    
    if (s_is_ap_mode) {
        esp_wifi_stop();
        if (s_ap_netif) {
            esp_netif_destroy(s_ap_netif);
            s_ap_netif = NULL;
        }
        s_is_ap_mode = false;
    }
    
    s_sta_netif = esp_netif_create_default_wifi_sta();
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = {0},
            .password = {0},
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    strncpy((char*)wifi_config.sta.ssid, s_stored_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, s_stored_password, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    ESP_LOGI(TAG, "STA mode started, connecting to: %s", s_stored_ssid);
    
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          pdMS_TO_TICKS(30000));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi successfully");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        return ESP_FAIL;
    }
}

// 保存WiFi配置到NVS
static esp_err_t save_wifi_config_to_nvs(const char* ssid, const char* password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        return err;
    }
    
    uint8_t configured = 1;
    err = nvs_set_u8(nvs_handle, NVS_KEY_WIFI_CONF_SAVED, configured);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to save config flag: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi configuration saved to NVS");
    return ESP_OK;
}

// 从NVS加载WiFi配置
static esp_err_t load_wifi_config_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found: %s", esp_err_to_name(err));
        return err;
    }
    
    uint8_t configured = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_WIFI_CONF_SAVED, &configured);
    if (err != ESP_OK || configured == 0) {
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "No WiFi configuration found in NVS");
        return ESP_FAIL;
    }
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to get SSID size: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, s_stored_ssid, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to get SSID: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, s_stored_password, &required_size);
        if (err != ESP_OK) {
            s_stored_password[0] = '\0';
        }
    }
    
    nvs_close(nvs_handle);
    
    s_wifi_configured = true;
    ESP_LOGI(TAG, "Loaded WiFi configuration from NVS: SSID=%s", s_stored_ssid);
    
    return ESP_OK;
}

// WiFi核心任务函数
static void wifi_core_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi core task started");
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 从NVS加载设备名称
    nvs_handle_t nvs_handle;
    ret = nvs_open("system", NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        size_t size = sizeof(s_device_name);
        nvs_get_str(nvs_handle, "device_name", s_device_name, &size);
        nvs_close(nvs_handle);
    }
    if (strlen(s_device_name) == 0) {
        snprintf(s_device_name, sizeof(s_device_name), "LED-Controller-%04X", (unsigned int)(esp_random() & 0xFFFF));
        
        nvs_handle_t nvs;
        if (nvs_open("system", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "device_name", s_device_name);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    
    // 创建事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    
    // WiFi初始化
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册WiFi事件处理
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    
    // 加载WiFi配置
    if (load_wifi_config_from_nvs() == ESP_OK) {
        ESP_LOGI(TAG, "Found saved WiFi configuration, attempting to connect...");
        if (start_sta_mode() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to connect to saved WiFi, switching to AP mode");
            start_ap_mode();
        }
    } else {
        ESP_LOGI(TAG, "No saved WiFi configuration, starting AP mode");
        start_ap_mode();
    }
    
    // 任务主循环
    while (1) {
        if (!s_is_ap_mode && s_wifi_configured) {
            if (!wifi_core_is_connected()) {
                ESP_LOGW(TAG, "WiFi disconnected, attempting to reconnect...");
                esp_wifi_connect();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// 启动WiFi核心任务
esp_err_t wifi_core_start(const wifi_core_config_t *wifi_cfg, const web_core_config_t *web_cfg)
{
    if (wifi_core_task_handle != NULL) {
        ESP_LOGE(TAG, "WiFi core task already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    memcpy(&s_wifi_config, wifi_cfg, sizeof(wifi_core_config_t));
    memcpy(&s_web_config, web_cfg, sizeof(web_core_config_t));
    
    if (strlen(wifi_cfg->device_name) > 0) {
        strncpy(s_device_name, wifi_cfg->device_name, sizeof(s_device_name) - 1);
        
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("system", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_set_str(nvs_handle, "device_name", s_device_name);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }
    
    ESP_LOGI(TAG, "Starting WiFi core task...");
    ESP_LOGI(TAG, "  Device Name: %s", s_device_name);
    ESP_LOGI(TAG, "  Web Port: %d", s_web_config.port);
    
    BaseType_t result = xTaskCreatePinnedToCore(
        wifi_core_task,
        "wifi_core_task",
        8192,
        NULL,
        5,
        &wifi_core_task_handle,
        0
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi core task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "WiFi core task started successfully");
    return ESP_OK;
}

// 停止WiFi核心任务
esp_err_t wifi_core_stop(void)
{
    if (wifi_core_task_handle == NULL) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping WiFi core task...");
    
    stop_web_server();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    
    vTaskDelete(wifi_core_task_handle);
    wifi_core_task_handle = NULL;
    
    ESP_LOGI(TAG, "WiFi core task stopped");
    return ESP_OK;
}

// 获取WiFi连接状态
bool wifi_core_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// 获取设备IP地址
esp_err_t wifi_core_get_ip_address(char *ip_buffer, size_t buffer_size)
{
    if (s_is_ap_mode) {
        if (ip_buffer && buffer_size >= 16) {
            strncpy(ip_buffer, "192.168.4.1", buffer_size);
        }
        return ESP_OK;
    }
    
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        return ESP_FAIL;
    }
    
    if (ip_buffer && buffer_size >= 16) {
        snprintf(ip_buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    }
    
    return ESP_OK;
}

// 检查是否为AP模式
bool wifi_core_is_ap_mode(void)
{
    return s_is_ap_mode;
}

// 重置WiFi配置（恢复到配网模式）
esp_err_t wifi_core_reset_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    nvs_set_u8(nvs_handle, NVS_KEY_WIFI_CONF_SAVED, 0);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    s_wifi_configured = false;
    memset(s_stored_ssid, 0, sizeof(s_stored_ssid));
    memset(s_stored_password, 0, sizeof(s_stored_password));
    
    esp_restart();
    
    return ESP_OK;
}