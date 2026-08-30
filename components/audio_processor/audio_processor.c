#include "audio_processor.h"
#include "inmp441_mic.h"
#include "utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-DSP库
#include "dsps_fft2r.h"
#include "dsps_wind.h"

static const char *TAG = "AUDIO_PROC";

// 全局麦克风配置
static mic_config_t mic_config = MIC_DEFAULT_CONFIG();

// FFT缓冲区
static float fft_input[FFT_SIZE * 2];  // 复数数组：实部+虚部
//static float fft_output[FFT_SIZE];     // 可保留或移除
static float fft_magnitude[FFT_OUTPUT_SIZE + 1]; // 包含直流分量和奈奎斯特频率点

// 初始化FFT处理器和麦克风
esp_err_t fft_processor_init(fft_processor_t *processor, int sample_rate) {
    if (!processor) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "初始化FFT处理器");
    ESP_LOGI(TAG, "  使用ESP-DSP库");
    ESP_LOGI(TAG, "  FFT大小: %d", FFT_SIZE);
    ESP_LOGI(TAG, "  频带数量: %d", NUM_FREQ_BANDS);
    ESP_LOGI(TAG, "  采样率: %d Hz", sample_rate);
    
    memset(processor, 0, sizeof(fft_processor_t));
    processor->sample_rate = sample_rate;
    
    // 初始化ESP-DSP库
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-DSP初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 初始化麦克风
    mic_config.sample_rate = sample_rate;
    ret = mic_init(&mic_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 初始化频带能量
    for (int i = 0; i < NUM_FREQ_BANDS; i++) {
        processor->frequency_bands[i] = 0;
        processor->peak_values[i] = 0;
        processor->peak_times[i] = 0;
    }
    
    ESP_LOGI(TAG, "FFT处理器初始化成功");
    return ESP_OK;
}

// 从麦克风读取音频数据
static esp_err_t read_microphone_data(int16_t *buffer, int samples) {
    return mic_read(buffer, samples);
}

// 一阶高通滤波器，截止频率约20Hz
static void high_pass_filter(int16_t *buffer, int size, float cutoff_freq, float sample_rate) {
    static float prev_input = 0;
    static float prev_output = 0;
    
    // 计算滤波器系数
    float dt = 1.0f / sample_rate;
    float RC = 1.0f / (2 * M_PI * cutoff_freq);
    float alpha = RC / (RC + dt);
    
    for (int i = 0; i < size; i++) {
        float input = buffer[i];
        float output = alpha * prev_output + alpha * (input - prev_input);
        
        buffer[i] = (int16_t)output;
        prev_input = input;
        prev_output = output;
    }
}

// 使用ESP-DSP进行FFT计算
static void compute_fft_with_dsp(const int16_t *input, float *magnitude) {
    // 1. 准备复数输入数组（重要：ESP-DSP的fft2r需要这种格式）
    // fft_input 必须是 float 数组，长度为 FFT_SIZE * 2（实部+虚部）
    
    for (int i = 0; i < FFT_SIZE; i++) {
        fft_input[i * 2] = (float)input[i];     // 实部：直接使用原始16位整数
        fft_input[i * 2 + 1] = 0.0f;            // 虚部：初始化为0
    }
    
    // 2. （可选）在此处应用窗函数到实部
    // for (int i = 0; i < FFT_SIZE; i++) {
    //     float window = 0.5f * (1.0f - cosf(2 * M_PI * i / (FFT_SIZE - 1))); // 汉宁窗
    //     fft_input[i * 2] *= window;
    // }
    
    // 3. 执行FFT（原地计算）
    dsps_fft2r_fc32(fft_input, FFT_SIZE);
    
    // 4. 位反转（ESP-DSP要求）
    dsps_bit_rev_fc32(fft_input, FFT_SIZE);
    
    // 5. 计算幅度谱（仅前FFT_SIZE/2+1个点，因为对称）
    // 注意：这里i从0开始，包含直流分量
    for (int i = 0; i <= FFT_OUTPUT_SIZE; i++) { // 包含第FFT_SIZE/2个点
        float real = fft_input[i * 2];
        float imag = fft_input[i * 2 + 1];
        // 计算幅度，并缩放到合理范围（避免过大值）
        magnitude[i] = sqrtf(real * real + imag * imag) / FFT_SIZE;
    }
    
    // 6. 调试：验证FFT结果
   /*static int fft_debug_cnt = 0;
    if (fft_debug_cnt++ % 20 == 0) {
        ESP_LOGW(TAG, "FFT幅度（前5个点）:");
        for (int i = 0; i < 5 && i <= FFT_OUTPUT_SIZE; i++) {
            ESP_LOGW(TAG, "  mag[%d]=%.6f", i, magnitude[i]);
        }
        // 检查输入范围
        int16_t max_val = 0;
        for (int i = 0; i < 10; i++) {
            if (abs(input[i]) > max_val) max_val = abs(input[i]);
        }
        ESP_LOGW(TAG, "前10个音频样本最大绝对值: %d", max_val);
    }*/
}


// 处理音频数据（从麦克风读取）
esp_err_t fft_processor_process(fft_processor_t *processor, int16_t *audio_buffer, int buffer_size) {
    if (!processor || buffer_size < FFT_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 1. 从麦克风读取数据
    esp_err_t ret = read_microphone_data(audio_buffer, FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取麦克风数据失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 2. 应用高通滤波器
    high_pass_filter(audio_buffer, FFT_SIZE, 20.0f, (float)processor->sample_rate);
    
    // 3. 计算FFT幅度谱
    compute_fft_with_dsp(audio_buffer, fft_magnitude);
    
    // 4. 计算频带能量（简化版本，移除LED相关逻辑）
    float freq_resolution = (float)processor->sample_rate / FFT_SIZE;
    
    // 对数频带边界
    float band_edges[NUM_FREQ_BANDS + 1];
    float log_min = log10f(20.0f);
    float log_max = log10f(8000.0f);
    float log_step = (log_max - log_min) / NUM_FREQ_BANDS;
    
    for (int i = 0; i <= NUM_FREQ_BANDS; i++) {
        band_edges[i] = powf(10.0f, log_min + i * log_step);
    }
    
    // 重置总能量
    processor->total_energy = 0;
    
    for (int band = 0; band < NUM_FREQ_BANDS; band++) {
        float band_energy = 0;
        int count = 0;

        // 计算频带能量
        int start_bin = (int)(band_edges[band] / freq_resolution);
        int end_bin = (int)(band_edges[band + 1] / freq_resolution);
        
        start_bin = (start_bin < 0) ? 0 : start_bin;
        end_bin = (end_bin > FFT_OUTPUT_SIZE) ? FFT_OUTPUT_SIZE : end_bin;
        
        for (int bin = start_bin; bin < end_bin; bin++) {
            band_energy += fft_magnitude[bin];
            count++;
        }
        
        // 计算平均能量并放大
        if (count > 0) {
            band_energy /= count;
            band_energy *= 100.0f;
        }
        
        // 指数平滑
        processor->frequency_bands[band] = 
            0.2f * processor->frequency_bands[band] + 0.8f * band_energy;
        
        // 累加总能量
        processor->total_energy += processor->frequency_bands[band];
    }

    // 计算光谱质心（基于频带能量和对数频带中心）
    if (processor->total_energy > 0) {
        float centroid_sum = 0;
        for (int i = 0; i < NUM_FREQ_BANDS; i++) {
            float center_freq = sqrtf(band_edges[i] * band_edges[i + 1]);
            centroid_sum += center_freq * processor->frequency_bands[i];
        }
        processor->spectral_centroid = centroid_sum / processor->total_energy;
    } else {
        processor->spectral_centroid = 0.0f;
    }
    
    return ESP_OK;
}

// 返回频带数组（只读）
const float* fft_get_frequency_bands(fft_processor_t *processor) {
    if (!processor) return NULL;
    return processor->frequency_bands;
}

// 返回总能量
float fft_get_total_energy(fft_processor_t *processor) {
    if (!processor) return 0.0f;
    return processor->total_energy;
}

// 返回光谱质心（Hz）
float fft_get_spectral_centroid(fft_processor_t *processor) {
    if (!processor) return 0.0f;
    return processor->spectral_centroid;
}