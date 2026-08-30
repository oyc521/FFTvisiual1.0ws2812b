#ifndef __WIFI_CORE_H__
#define __WIFI_CORE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi配置
typedef struct {
    char ssid[32];          // WiFi SSID
    char password[64];      // WiFi密码
    char device_name[32];   // 设备名称
} wifi_core_config_t;

// Web服务器配置
typedef struct {
    uint16_t port;          // HTTP服务器端口
    bool enable_websocket;  // 是否启用WebSocket
} web_core_config_t;

/**
 * @brief 启动WiFi核心任务
 * 
 * @param wifi_cfg WiFi配置
 * @param web_cfg Web服务器配置
 * @return esp_err_t 
 */
esp_err_t wifi_core_start(const wifi_core_config_t *wifi_cfg, const web_core_config_t *web_cfg);

/**
 * @brief 停止WiFi核心任务
 * 
 * @return esp_err_t 
 */
esp_err_t wifi_core_stop(void);

/**
 * @brief 获取WiFi连接状态
 * 
 * @return true 已连接
 * @return false 未连接
 */
bool wifi_core_is_connected(void);

/**
 * @brief 获取设备IP地址
 * 
 * @param ip_buffer 存储IP地址的缓冲区
 * @param buffer_size 缓冲区大小
 * @return esp_err_t 
 */
esp_err_t wifi_core_get_ip_address(char *ip_buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_CORE_H__ */
