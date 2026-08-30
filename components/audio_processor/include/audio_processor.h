#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// FFT配置
#define FFT_SIZE          512      // FFT点数
#define FFT_OUTPUT_SIZE   (FFT_SIZE / 2)  // 输出点数（对称）
#define NUM_FREQ_BANDS    8        // 频带数量

// FFT处理器结构体
typedef struct {
    int sample_rate;               // 采样率
    float frequency_bands[NUM_FREQ_BANDS];  // 频带能量
    float peak_values[NUM_FREQ_BANDS];      // 峰值
    uint32_t peak_times[NUM_FREQ_BANDS];    // 峰值时间
    float total_energy;            // 总能量
    float spectral_centroid;       // 频谱质心
} fft_processor_t;

// 函数声明
esp_err_t fft_processor_init(fft_processor_t *processor, int sample_rate);
esp_err_t fft_processor_process(fft_processor_t *processor, int16_t *audio_buffer, int buffer_size);
esp_err_t fft_processor_deinit(fft_processor_t *processor);

const float* fft_get_frequency_bands(fft_processor_t *processor);
float fft_get_spectral_centroid(fft_processor_t *processor);
float fft_get_total_energy(fft_processor_t *processor);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PROCESSOR_H */