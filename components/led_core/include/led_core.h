#ifndef __LED_CORE_H__
#define __LED_CORE_H__

#include "esp_err.h"
#include "led_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED配置
 */
typedef struct {
    int gpio_pin;           // GPIO引脚
    int num_leds;           // LED数量
    int brightness;         // 初始亮度
} led_core_config_t;

// 渲染统计
typedef struct {
    uint32_t frame_rate;      // 当前帧率 (FPS)
    uint32_t total_frames;    // 总渲染帧数
    uint32_t missed_frames;   // 丢帧数
    float average_fps;        // 平均FPS
    led_mode_t current_mode;  // 当前模式
    bool has_audio_data;      // 是否有音频数据
} led_core_stats_t;

/**
 * @brief 启动LED核心任务 (运行在Core 1)
 * 
 * @param config LED配置
 * @return esp_err_t 执行结果
 */
esp_err_t led_core_start(const led_core_config_t *config);

/**
 * @brief 停止LED核心任务
 * 
 * @return esp_err_t 执行结果
 */
esp_err_t led_core_stop(void);

/**
 * @brief 获取当前LED模式
 * 
 * @return led_mode_t 当前模式
 */
led_mode_t led_core_get_current_mode(void);

/**
 * @brief 获取渲染统计信息
 * 
 * @param stats 统计信息结构体指针
 * @return esp_err_t 执行结果
 */
esp_err_t led_core_get_stats(led_core_stats_t *stats);

/**
 * @brief 手动触发测试效果
 * 
 * @param test_mode 测试模式
 * @return esp_err_t 执行结果
 */
esp_err_t led_core_test_effect(led_mode_t test_mode);

#ifdef __cplusplus
}
#endif

#endif /* __LED_CORE_H__ */

