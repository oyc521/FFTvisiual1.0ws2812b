// components/inmp441_mic/include/inmp441_mic.h
#ifndef INMP441_MIC_H
#define INMP441_MIC_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

// INMP441 麦克风配置
typedef struct {
    int sample_rate;      // 采样率 (Hz)
    int buffer_size;      // 缓冲区大小
    int dma_buf_count;    // DMA缓冲区数量
    int dma_buf_len;      // DMA缓冲区长度
    int i2s_port;         // I2S端口号
} mic_config_t;

// 默认配置
#define MIC_DEFAULT_CONFIG() { \
    .sample_rate = 16000,      /* 16kHz采样率 */ \
    .buffer_size = 1024,       \
    .dma_buf_count = 4,        \
    .dma_buf_len = 256,        \
    .i2s_port = 0              \
}

// INMP441 引脚定义（ESP32-S3）
// 请根据你的实际连接修改这些引脚
#define MIC_BCK_GPIO   GPIO_NUM_15  // 位时钟（BCK/SCK）
#define MIC_WS_GPIO    GPIO_NUM_16  // 字选择（WS/LRCLK）
#define MIC_DATA_GPIO  GPIO_NUM_17  // 数据线（SD/DOUT）

// 函数声明
esp_err_t mic_init(const mic_config_t *config);
esp_err_t mic_read(int16_t *buffer, size_t samples);
esp_err_t mic_deinit(void);
esp_err_t mic_test_hardware(void);  // 新增硬件测试函数
int mic_get_sample_rate(void);
size_t mic_get_buffer_size(void);

#ifdef __cplusplus
}
#endif

#endif /* INMP441_MIC_H */