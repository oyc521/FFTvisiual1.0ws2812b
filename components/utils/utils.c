// components/utils/utils.c
#include "utils.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "UTILS";

// 映射浮点数
float map_float(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// 映射整数
int32_t map_int(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// 约束浮点数范围
float constrain_float(float x, float min_val, float max_val) {
    if (x < min_val) return min_val;
    if (x > max_val) return max_val;
    return x;
}

// 约束整数范围
int32_t constrain_int(int32_t x, int32_t min_val, int32_t max_val) {
    if (x < min_val) return min_val;
    if (x > max_val) return max_val;
    return x;
}

// 获取数组最大值
float array_max(const float *arr, int size) {
    if (size <= 0) return 0;
    float max_val = arr[0];
    for (int i = 1; i < size; i++) {
        if (arr[i] > max_val) max_val = arr[i];
    }
    return max_val;
}

// 应用窗函数
void apply_window(float *data, int size, const char *window_type) {
    if (strcmp(window_type, "hann") == 0) {
        // 汉宁窗
        for (int i = 0; i < size; i++) {
            float window = 0.5 * (1 - cosf(2 * M_PI * i / (size - 1)));
            data[i] *= window;
        }
    } else if (strcmp(window_type, "hamming") == 0) {
        // 汉明窗
        for (int i = 0; i < size; i++) {
            float window = 0.54 - 0.46 * cosf(2 * M_PI * i / (size - 1));
            data[i] *= window;
        }
    }
    // 其他窗函数...
}

// 打印数组（调试用）
void print_array(const char *label, const float *arr, int size) {
    if (size > 20) {
        ESP_LOGI(TAG, "%s: [只显示前20个]", label);
        size = 20;
    }
    
    char buffer[256] = {0};
    char temp[16];
    
    strcpy(buffer, label);
    strcat(buffer, ": [");
    
    for (int i = 0; i < size; i++) {
        sprintf(temp, "%.2f", arr[i]);
        strcat(buffer, temp);
        if (i < size - 1) strcat(buffer, ", ");
    }
    strcat(buffer, "]");
    
    ESP_LOGI(TAG, "%s", buffer);
}

// 获取当前时间（毫秒）
uint64_t get_time_ms(void) {
    return esp_timer_get_time() / 1000;
}

// 延时函数（非阻塞）
void delay_ms(uint32_t ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}