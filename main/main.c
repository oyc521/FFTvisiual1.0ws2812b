#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "led_controller.h"
#include "audio_processor.h"
#include "inmp441_mic.h"
#include "utils.h"
#include "dual_core_com.h"
#include "wifi_core.h"
#include "led_core.h"

static const char *TAG = "MAIN";

// ============================================================
// ===== 请将下面的 WiFi 信息修改为你自己的网络配置 =====
// ============================================================
// 方式一：直接修改下方的 ssid 和 password（适合初学者）
// 方式二：通过 menuconfig (idf.py menuconfig) 进行配置（推荐）
// ============================================================

// 全局配置
static wifi_core_config_t wifi_cfg = {
    .ssid = "YOUR_WIFI_SSID",       // TODO: 修改为你的 WiFi 名称
    .password = "YOUR_WIFI_PASSWORD", // TODO: 修改为你的 WiFi 密码
    .device_name = "ESP32-S3-Visualizer"
};

static web_core_config_t web_cfg = {
    .port = 80,
    .enable_websocket = true
};

static led_core_config_t led_cfg = {
    .gpio_pin = 5,      // LED GPIO引脚（根据实际修改）
    .num_leds = 20,      // LED数量
    .brightness = 50     // 亮度50%
};

// 音频处理器实例
static fft_processor_t audio_processor;

// 系统信息监控任务
void system_monitor_task(void *pvParameter) {
    int counter = 0;
    
    while (1) {
        if (counter++ % 60 == 0) {
            // 获取双核通信状态
            core_status_t status;
            dual_core_com_get_status(&status);
            
            ESP_LOGI(TAG, "================== 系统状态 ==================");
            ESP_LOGI(TAG, "  当前模式: %d", status.current_mode);
            ESP_LOGI(TAG, "  WiFi连接: %s", status.wifi_connected ? "已连接" : "未连接");
            ESP_LOGI(TAG, "  LED帧数: %lu", status.led_frame_count);
            ESP_LOGI(TAG, "  核心0: WiFi控制 | 核心1: LED渲染");
            ESP_LOGI(TAG, "  空闲内存: %d字节", esp_get_free_heap_size());
            ESP_LOGI(TAG, "  最小空闲内存: %d字节", esp_get_minimum_free_heap_size());
            
            #ifdef CONFIG_SPIRAM_SUPPORT
            ESP_LOGI(TAG, "  PSRAM空闲: %d字节", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            #endif
            
            ESP_LOGI(TAG, "============================================");
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// 音频处理任务（核心0）
void audio_processing_task(void *pvParameter) {
    ESP_LOGI(TAG, "音频处理任务开始");
    
    // 初始化FFT处理器（会自动初始化麦克风）
    if (fft_processor_init(&audio_processor, 44100) != ESP_OK) {
        ESP_LOGE(TAG, "FFT处理器初始化失败");
        vTaskDelete(NULL);
    }
    
    // 设置默认LED模式
    led_set_mode(MODE_EXPLOSION);
    
    ESP_LOGI(TAG, "系统准备就绪，开始音频可视化...");
    ESP_LOGI(TAG, "当前模式：爆炸效果");
    ESP_LOGI(TAG, "使用INMP441麦克风采集实时音频");
    
    int16_t audio_buffer[FFT_SIZE];
    float volume_smooth = 0;
    float bands[NUM_FREQ_BANDS];
    
    while (1) {
        // 1. 处理音频数据（从麦克风读取）
        esp_err_t ret = fft_processor_process(&audio_processor, audio_buffer, FFT_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "音频处理失败，重试...");
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }
        
        // 2. 获取频带能量
        const float *raw_bands = fft_get_frequency_bands(&audio_processor);
        float total_energy = fft_get_total_energy(&audio_processor);
        
        // 3. 复制频带数据到本地数组
        memcpy(bands, raw_bands, NUM_FREQ_BANDS * sizeof(float));
        
        // 4. 平滑音量值
        volume_smooth = 0.9f * volume_smooth + 0.1f * total_energy;
        
        // 5. 通过双核通信发送音频数据到LED核心
        dual_core_com_send_audio_data(bands, NUM_FREQ_BANDS, volume_smooth);
        
        // 6. 调试信息（每30帧打印一次）
        static int debug_counter = 0;
        if (debug_counter++ % 30 == 0) {
            ESP_LOGI(TAG, "音频能量: 总=%.2f, 高频=%.2f", 
                    total_energy, 
                    bands[NUM_FREQ_BANDS-1] + bands[NUM_FREQ_BANDS-2]);
        }
        
        // 7. 控制帧率（约43FPS）
        //vTaskDelay(23 / portTICK_PERIOD_MS);
        vTaskDelay(10 / portTICK_PERIOD_MS); // 改为10ms（100FPS）
    }
    
    // 清理资源
    fft_processor_deinit(&audio_processor);
    vTaskDelete(NULL);
}

void app_main(void) {
    // 1. 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 2. 打印系统信息
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "   ESP32-S3双核音频可视化系统");
    ESP_LOGI(TAG, "   INMP441麦克风 + ESP-DSP库");
    ESP_LOGI(TAG, "   核心0: WiFi控制 + 音频处理");
    ESP_LOGI(TAG, "   核心1: LED渲染");
    ESP_LOGI(TAG, "==========================================");
    
    // 3. 打印芯片信息
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    ESP_LOGI(TAG, "硬件信息:");
    ESP_LOGI(TAG, "  芯片型号: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "  核心数: %d", chip_info.cores);
    ESP_LOGI(TAG, "  芯片版本: %d", chip_info.revision);
    ESP_LOGI(TAG, "  FreeRTOS版本: %s", tskKERNEL_VERSION_NUMBER);
    ESP_LOGI(TAG, "  SDK版本: %s", esp_get_idf_version());
    
    // 打印MAC地址
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "  MAC地址: %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // 4. 初始化双核通信
    ESP_LOGI(TAG, "步骤1: 初始化双核通信...");
    if (dual_core_com_init() != ESP_OK) {
        ESP_LOGE(TAG, "双核通信初始化失败");
        return;
    }
    ESP_LOGI(TAG, "双核通信初始化完成");
    
    // 5. 启动LED核心（Core 1）
    ESP_LOGI(TAG, "步骤2: 启动LED核心任务 (Core 1)...");
    if (led_core_start(&led_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LED核心任务启动失败");
        return;
    }
    ESP_LOGI(TAG, "LED核心任务启动成功");
    
    // 6. 启动WiFi核心（Core 0）
    ESP_LOGI(TAG, "步骤3: 启动WiFi核心任务 (Core 0)...");
    if (wifi_core_start(&wifi_cfg, &web_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi核心任务启动失败");
        led_core_stop();
        return;
    }  
    ESP_LOGI(TAG, "WiFi核心任务启动成功");
    
    // 7. 创建音频处理任务
    ESP_LOGI(TAG, "步骤4: 启动音频处理任务...");
    xTaskCreate(audio_processing_task, "audio_task", 8192, NULL, 10, NULL);
    
    // 8. 创建系统监控任务
    ESP_LOGI(TAG, "步骤5: 启动系统监控任务...");
    xTaskCreate(system_monitor_task, "monitor_task", 4096, NULL, 1, NULL);
    
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "系统启动完成！");
    ESP_LOGI(TAG, "请按以下步骤操作：");
    ESP_LOGI(TAG, "1. 等待WiFi连接成功");
    ESP_LOGI(TAG, "2. 查看串口日志获取IP地址");
    ESP_LOGI(TAG, "3. 浏览器访问 http://[设备IP]");
    ESP_LOGI(TAG, "4. 音频可视化已自动开始");
    ESP_LOGI(TAG, "==========================================");
    
    // 主循环
    while (1) {
        // 可以在这里添加其他系统级功能
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}