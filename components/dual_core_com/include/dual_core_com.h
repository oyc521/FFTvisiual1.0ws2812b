#ifndef __DUAL_CORE_COM_H__
#define __DUAL_CORE_COM_H__

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "led_controller.h"
#include "audio_processor.h"

#ifdef __cplusplus
extern "C" {
#endif

// 命令类型定义
typedef enum {
    CMD_MODE_CHANGE,      // 模式切换命令
    CMD_BRIGHTNESS_UP,    // 增加亮度
    CMD_BRIGHTNESS_DOWN,  // 降低亮度
    CMD_TEST_RAINBOW,     // 测试彩虹
    CMD_SET_PARAM,        // 设置参数
    CMD_GET_STATUS        // 获取状态
} core_command_type_t;

// 命令数据结构
typedef struct {
    core_command_type_t type;
    union {
        led_mode_t mode;              // 模式值
        struct {
            char key[32];             // 参数键
            int value;                // 参数值
        } param;
    } data;
} core_command_t;

// 状态数据结构
typedef struct {
    led_mode_t current_mode;          // 当前模式
    bool wifi_connected;              // WiFi连接状态
    uint32_t led_frame_count;         // LED帧计数
    uint32_t free_heap_size;          // 空闲堆内存
} core_status_t;

// 音频数据结构
typedef struct {
    float frequency_bands[NUM_FREQ_BANDS];
    float total_energy;
    float spectral_centroid;
    uint32_t timestamp;
} audio_data_t;

/**
 * @brief 初始化双核通信
 * 
 * @return esp_err_t 
 */
esp_err_t dual_core_com_init(void);

/**
 * @brief 发送命令到另一个核心
 * 
 * @param cmd 命令数据
 * @param timeout 超时时间
 * @return esp_err_t 
 */
esp_err_t dual_core_com_send_command(core_command_t *cmd, TickType_t timeout);

/**
 * @brief 从队列接收命令
 * 
 * @param cmd 存储命令的指针
 * @param timeout 超时时间
 * @return esp_err_t 
 */
esp_err_t dual_core_com_receive_command(core_command_t *cmd, TickType_t timeout);

/**
 * @brief 获取当前状态
 * 
 * @param status 状态结构体指针
 */
void dual_core_com_get_status(core_status_t *status);

/**
 * @brief 更新状态信息
 * 
 * @param new_mode 新模式（如果不需要更新，传-1）
 * @param frame_count 帧计数
 */
void dual_core_com_update_status(led_mode_t new_mode, uint32_t frame_count);

/**
 * @brief 设置WiFi连接状态
 * 
 * @param connected 连接状态
 */
void dual_core_com_set_wifi_status(bool connected);

/**
 * @brief 发送音频数据到另一个核心
 * 
 * @param bands 频率带数组
 * @param num_bands 频率带数量
 * @param total_energy 总能量值
 * @return esp_err_t 
 */
esp_err_t dual_core_com_send_audio_data(const float *bands, int num_bands, float total_energy);
esp_err_t dual_core_com_receive_audio_data(audio_data_t *audio_data, TickType_t timeout);

// ==================== 调试监控函数（已注释，后续启用） ====================
/*
// 调试统计结构
typedef struct {
    uint32_t audio_send_success;     // 音频数据发送成功次数
    uint32_t audio_send_dropped;     // 音频数据丢弃次数
    uint32_t audio_receive_success;  // 音频数据接收成功次数
    uint32_t audio_receive_timeout;  // 音频数据接收超时次数
    uint32_t command_send_success;   // 命令发送成功次数
    uint32_t command_receive_success;// 命令接收成功次数
} dual_core_stats_t;

// 获取音频队列状态
UBaseType_t dual_core_com_get_audio_queue_count(void);
UBaseType_t dual_core_com_get_audio_queue_space(void);

// 获取命令队列状态
UBaseType_t dual_core_com_get_command_queue_count(void);
UBaseType_t dual_core_com_get_command_queue_space(void);

// 清空队列（用于调试）
void dual_core_com_clear_audio_queue(void);
void dual_core_com_clear_command_queue(void);

// 获取调试统计信息
void dual_core_com_get_debug_stats(dual_core_stats_t *stats);

// 重置调试统计
void dual_core_com_reset_debug_stats(void);

// 打印队列状态信息
void dual_core_com_print_status(void);
*/

/**
 * @brief 清理双核通信资源
 * 
 * @return esp_err_t 执行结果
 */
esp_err_t dual_core_com_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __DUAL_CORE_COM_H__ */