// components/utils/include/utils.h
#ifndef __UTILS_H__
#define __UTILS_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 数学辅助函数
float map_float(float x, float in_min, float in_max, float out_min, float out_max);
int32_t map_int(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max);
float constrain_float(float x, float min_val, float max_val);
int32_t constrain_int(int32_t x, int32_t min_val, int32_t max_val);

// 数组操作
float array_max(const float *arr, int size);
float array_min(const float *arr, int size);
float array_mean(const float *arr, int size);
float array_std(const float *arr, int size);

// 数字信号处理辅助
void apply_window(float *data, int size, const char *window_type);
void normalize_array(float *data, int size);
void smooth_array(float *data, int size, float alpha);

// 调试工具
void print_array(const char *label, const float *arr, int size);
void print_hex(const char *label, const uint8_t *data, int length);

// 时间工具
uint64_t get_time_ms(void);
uint64_t get_time_us(void);
void delay_ms(uint32_t ms);

#endif // __UTILS_H__;
