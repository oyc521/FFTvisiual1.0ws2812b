#include "led_core.h"
#include "led_controller.h"
#include "dual_core_com.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LED_CORE";

// 全局变量
static TaskHandle_t led_core_task_handle = NULL;
static led_core_config_t s_led_config;
static led_mode_t current_mode = MODE_METEOR_PULSE;
static uint32_t frame_count = 0;
//static uint32_t last_stat_time = 0;
static uint32_t frame_rate = 0;

// 音频数据缓冲区
static audio_data_t current_audio_data;
static bool has_audio_data = false;

// 渲染统计
static struct {
    uint32_t total_frames;
    uint32_t missed_frames;
    uint32_t last_frame_time;
    float average_fps;
} render_stats = {0};

// 模式切换处理函数
static void handle_mode_change(led_mode_t new_mode) {
    if (new_mode >= MODE_SPECTRUM && new_mode <= MODE_OFF) {
        current_mode = new_mode;
        led_set_mode(new_mode);
        ESP_LOGI(TAG, "模式切换到: %d", new_mode);
        
        // 更新双核通信状态
        dual_core_com_update_status(current_mode, frame_count);
    }
}

// 命令处理函数
static void handle_command(core_command_t *cmd) {
    if (!cmd) return;
    
    switch (cmd->type) {
        case CMD_MODE_CHANGE:
            handle_mode_change(cmd->data.mode);
            break;
            
        case CMD_BRIGHTNESS_UP:
            // 这里可以添加亮度增加的逻辑
            ESP_LOGI(TAG, "亮度增加命令");
            break;
            
        case CMD_BRIGHTNESS_DOWN:
            // 这里可以添加亮度降低的逻辑
            ESP_LOGI(TAG, "亮度降低命令");
            break;
            
        case CMD_TEST_RAINBOW:
            // 测试彩虹效果
            ESP_LOGI(TAG, "测试彩虹效果");
            led_test_rainbow();
            break;
            
        default:
            ESP_LOGW(TAG, "未知命令类型: %d", cmd->type);
            break;
    }
}

// 更新渲染统计
static void update_render_stats(void) {
    uint32_t current_time = xTaskGetTickCount();
    
    render_stats.total_frames++;
    
    // 计算帧率
    if (current_time - render_stats.last_frame_time >= pdMS_TO_TICKS(1000)) {
        uint32_t frames_in_last_second = render_stats.total_frames - frame_count;
        frame_rate = frames_in_last_second;
        frame_count = render_stats.total_frames;
        render_stats.last_frame_time = current_time;
        
        // 计算平均FPS
        float time_seconds = pdTICKS_TO_MS(current_time) / 1000.0f;
        if (time_seconds > 0) {
            render_stats.average_fps = render_stats.total_frames / time_seconds;
        }
        
        // 定期打印统计信息
        static int log_counter = 0;
        if (log_counter++ % 10 == 0) {
            ESP_LOGI(TAG, "渲染统计: 帧率=%d FPS, 总帧数=%lu, 丢帧=%lu", 
                    frame_rate, render_stats.total_frames, render_stats.missed_frames);
        }
    }
}
// 修改 led_core.c 中的 handle_idle_animation 函数

// 添加一个静态的零数组作为后备数据
static float zero_frequency_bands[NUM_FREQ_BANDS] = {0};

static void handle_idle_animation(void) {
    static uint32_t idle_counter = 0;
    
    switch (current_mode) { 
        case MODE_RAINBOW:
            // 彩虹模式即使没有音频也继续
            led_update_visualization(NULL, 0);
            break;
            
        case MODE_METEOR_PULSE:
            // 流星脉冲模式如果没有音频数据，显示微弱的背景
            led_update_visualization(NULL, 0);
            break;

        case MODE_SPECTRUM:    
        case MODE_WATER_RIPPLE:
        case MODE_ENERGY_WAVE:
        case MODE_FIREWORKS:
        case MODE_RHYTHM_PULSE:
        case MODE_RHYTHM_BREATH:
        case MODE_RHYTHM_JUMP:
        case MODE_SPARKLE_RAINBOW:
        case MODE_EXPLOSION:
            // 这些增强效果也需要处理无音频数据的情况
            led_update_visualization(zero_frequency_bands, NUM_FREQ_BANDS);
            break;
            
        case MODE_OFF:
            // 关闭模式：清空LED
            led_clear_all();
            led_show();
            break;
            
        default:
            // 其他模式：保持当前显示或显示微弱的呼吸效果
            idle_counter++;
            float breath = (sinf(idle_counter * 0.1f) + 1.0f) * 0.1f;
            
            rgb_color_t color = {
                .r = (uint8_t)(50 * breath),
                .g = (uint8_t)(50 * breath),
                .b = (uint8_t)(50 * breath)
            };
            
            for (int i = 0; i < s_led_config.num_leds; i++) {
                if (i % 20 == (idle_counter / 10) % 20) {
                    led_set_pixel(i, color);
                }
            }
            led_show();
            break;
    }
}

// LED核心任务函数
static void led_core_task(void *pvParameters) {
    ESP_LOGI(TAG, "LED核心任务启动 (Core %d)", xPortGetCoreID());
    
    // 初始化LED控制器
    led_config_t led_cfg = {
        .gpio_pin = s_led_config.gpio_pin,
        .num_leds = s_led_config.num_leds,
        .brightness = s_led_config.brightness
    };
    
    esp_err_t ret = led_controller_init(&led_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED控制器初始化失败: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
    }
    
    // 设置初始模式
    led_set_mode(current_mode);
    ESP_LOGI(TAG, "初始模式设置为: %d", current_mode);
    
    // 初始化渲染统计
    render_stats.last_frame_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "LED核心任务就绪，等待音频数据...");
    
    // 主循环
    while (1) {
        // 1. 处理命令（非阻塞）
        core_command_t cmd;
        if (dual_core_com_receive_command(&cmd, 0) == ESP_OK) {
            handle_command(&cmd);
        }
        
        // 2. 接收音频数据（非阻塞）
        has_audio_data = false;
        if (dual_core_com_receive_audio_data(&current_audio_data, 0) == ESP_OK) {
            has_audio_data = true;
        }
        
        // 3. 渲染LED效果
        if (has_audio_data) {
            // 使用音频数据渲染
            ret = led_update_visualization(current_audio_data.frequency_bands, NUM_FREQ_BANDS);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "LED渲染失败: %s", esp_err_to_name(ret));
                render_stats.missed_frames++;
            }
        } else {
            // 无音频数据时的处理
            handle_idle_animation();
        }
        
        // 4. 更新渲染统计
        update_render_stats();
        
        // 5. 更新双核通信状态
        dual_core_com_update_status(current_mode, frame_count);
        
        // 6. 控制帧率（最小延迟1ms，让出CPU时间）
        // 注意：LED渲染本身可能需要时间，所以这里延迟很短
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 启动LED核心任务
esp_err_t led_core_start(const led_core_config_t *config) {
    if (led_core_task_handle != NULL) {
        ESP_LOGE(TAG, "LED核心任务已在运行");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 保存配置
    memcpy(&s_led_config, config, sizeof(led_core_config_t));
    
    ESP_LOGI(TAG, "启动LED核心任务...");
    ESP_LOGI(TAG, "  GPIO引脚: %d", s_led_config.gpio_pin);
    ESP_LOGI(TAG, "  LED数量: %d", s_led_config.num_leds);
    ESP_LOGI(TAG, "  亮度: %d%%", s_led_config.brightness);
    
    // 创建LED核心任务（绑定到Core 1）
    BaseType_t result = xTaskCreatePinnedToCore(
        led_core_task,      // 任务函数
        "led_core_task",    // 任务名称
        8192,               // 堆栈大小（LED效果可能需要较多栈空间）
        NULL,               // 参数
        8,                  // 优先级（较高优先级，确保流畅渲染）
        &led_core_task_handle, // 任务句柄
        1                   // 绑定到Core 1
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "创建LED核心任务失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "LED核心任务启动成功，运行在Core 1");
    return ESP_OK;
}

// 停止LED核心任务
esp_err_t led_core_stop(void) {
    if (led_core_task_handle == NULL) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "停止LED核心任务...");
    
    // 先关闭LED
    led_set_mode(MODE_OFF);
    led_clear_all();
    led_show();
    
    // 删除任务
    vTaskDelete(led_core_task_handle);
    led_core_task_handle = NULL;
    
    ESP_LOGI(TAG, "LED核心任务已停止");
    return ESP_OK;
}

// 获取当前模式
led_mode_t led_core_get_current_mode(void) {
    return current_mode;
}

// 获取渲染统计
esp_err_t led_core_get_stats(led_core_stats_t *stats) {
    if (!stats) return ESP_ERR_INVALID_ARG;
    
    stats->frame_rate = frame_rate;
    stats->total_frames = render_stats.total_frames;
    stats->missed_frames = render_stats.missed_frames;
    stats->average_fps = render_stats.average_fps;
    stats->current_mode = current_mode;
    stats->has_audio_data = has_audio_data;
    
    return ESP_OK;
}

// 手动触发测试效果
esp_err_t led_core_test_effect(led_mode_t test_mode) {
    if (test_mode < MODE_DEBUG || test_mode > MODE_OFF) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "手动触发测试效果: %d", test_mode);
    
    // 临时切换到测试模式
    led_mode_t original_mode = current_mode;
    current_mode = test_mode;
    led_set_mode(test_mode);
    
    // 生成测试音频数据
    float test_bands[NUM_FREQ_BANDS];
    for (int i = 0; i < NUM_FREQ_BANDS; i++) {
        // 生成测试频谱：正弦波形式
        test_bands[i] = sinf(i * 0.3f + (float)frame_count * 0.1f) * 0.5f + 0.5f;
        test_bands[i] *= 50.0f; // 放大到合适范围
    }
    
    // 渲染测试效果
    esp_err_t ret = led_update_visualization(test_bands, NUM_FREQ_BANDS);
    
    // 恢复原模式
    current_mode = original_mode;
    led_set_mode(original_mode);
    
    return ret;
}