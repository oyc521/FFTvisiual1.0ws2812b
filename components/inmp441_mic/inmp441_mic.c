#include "inmp441_mic.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "INMP441";
static i2s_chan_handle_t rx_handle = NULL;
static mic_config_t mic_cfg;

esp_err_t mic_init(const mic_config_t *config) {
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing INMP441 microphone...");
    ESP_LOGI(TAG, "  Sample rate: %d Hz", config->sample_rate);
    ESP_LOGI(TAG, "  Buffer size: %d", config->buffer_size);
    ESP_LOGI(TAG, "  I2S port: %d", config->i2s_port);
    
    // 保存配置
    mic_cfg = *config;
    
    // 1. 配置I2S通道（使用新的API）
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // 自动清除DMA缓冲区
    chan_cfg.dma_desc_num = config->dma_buf_count;  // 添加DMA缓冲区数量
    chan_cfg.dma_frame_num = config->dma_buf_len;   // 添加DMA帧数量
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 2. 配置标准模式 - 使用正确的INMP441配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = config->sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // 主时钟倍率
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,  // INMP441使用32位传输
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,  // 立体声模式
            .slot_mask = I2S_STD_SLOT_BOTH,     // 左右声道都使能
            .ws_width = 32,
            .ws_pol = false,
            .bit_shift = true,
            // .msb_right 字段可能在新版本中已移除或改名
        },
        .gpio_cfg = {
            .bclk = MIC_BCK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DATA_GPIO,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
                // .data_inv 字段可能已移除
            },
        },
    };
    
    // 3. 初始化通道
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }
    
    // 4. 启用通道
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "INMP441 microphone initialized successfully");
    return ESP_OK;
}

esp_err_t mic_read(int16_t *buffer, size_t samples) {
    if (!buffer || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!rx_handle) {
        ESP_LOGE(TAG, "I2S channel not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t bytes_read = 0;
    
    // INMP441是24位，但I2S传输使用32位
    // 每个样本需要2个32位（左右声道）
    size_t frames_needed = samples;
    size_t bytes_to_read = frames_needed * 2 * sizeof(int32_t);  // 立体声，每帧2个int32
    
    int32_t *temp_buffer = (int32_t *)malloc(bytes_to_read);
    if (!temp_buffer) {
        ESP_LOGE(TAG, "Memory allocation failed for temp buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // 读取原始32位数据
    esp_err_t ret = i2s_channel_read(rx_handle, temp_buffer, bytes_to_read, 
                                     &bytes_read, pdMS_TO_TICKS(100));
    
    if (ret != ESP_OK) {
        free(temp_buffer);
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    size_t frames_read = bytes_read / (2 * sizeof(int32_t));
    if (frames_read < frames_needed) {
        ESP_LOGW(TAG, "Incomplete read: %d/%d frames", frames_read, frames_needed);
    }
    
    // 调试：打印原始数据的前几个值
    /*static int debug_count = 0;
    if (debug_count++ % 100 == 0 && frames_read > 4) {
        ESP_LOGI(TAG, "原始32位数据（前4帧，每帧左右声道）:");
        for (int i = 0; i < 4 && i < frames_read; i++) {
            ESP_LOGI(TAG, "  帧[%d]: L=0x%08X (%d), R=0x%08X (%d)", 
                    i, 
                    temp_buffer[i*2], temp_buffer[i*2],
                    temp_buffer[i*2+1], temp_buffer[i*2+1]);
        }
    }*/
    
    // 转换数据：INMP441输出24位左对齐，在高24位
    // 我们只取左声道（偶数索引），右移8位得到有效16位数据
    for (size_t i = 0; i < frames_read && i < samples; i++) {
        // 获取左声道数据（偶数索引）
        int32_t raw_value = temp_buffer[i * 2];
        
        // INMP441输出24位左对齐在32位中
        // 方案1：右移16位直接得到16位（丢弃低16位）
        // 方案2：右移8位得到24位，再右移8位得到16位
        buffer[i] = (int16_t)(raw_value >> 16);  // 方法1：直接右移16位
        
        // 或者使用方法2（保留更多精度）：
        // int32_t value_24bit = raw_value >> 8;  // 得到24位有符号值
        // buffer[i] = (int16_t)(value_24bit >> 8);  // 转换为16位
    }
    
    // 如果读取的帧数不足，用0填充
    for (size_t i = frames_read; i < samples; i++) {
        buffer[i] = 0;
    }
    
    free(temp_buffer);
    
    // 调试：打印转换后的数据
    /*if (debug_count % 100 == 0 && frames_read > 5) {
        ESP_LOGI(TAG, "转换后的16位音频样本（前10个）:");
        for (int i = 0; i < 10 && i < samples; i++) {
            ESP_LOGI(TAG, "  [%d]: %d (0x%04X)", i, buffer[i], (uint16_t)buffer[i]);
        }
        
        // 检查是否有非零数据
        int non_zero_count = 0;
        for (int i = 0; i < 20 && i < samples; i++) {
            if (buffer[i] != 0) non_zero_count++;
        }
        ESP_LOGI(TAG, "前20个样本中非零值数量: %d", non_zero_count);
        
        // 计算平均值（用于检查是否有有效信号）
        int32_t sum = 0;
        for (int i = 0; i < 100 && i < samples; i++) {
            sum += abs(buffer[i]);
        }
        int avg = sum / (samples < 100 ? samples : 100);
        ESP_LOGI(TAG, "前100个样本绝对值的平均值: %d", avg);
    }
    */
    return ESP_OK;
}

esp_err_t mic_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing INMP441 microphone");
    if (rx_handle) {
        esp_err_t ret = i2s_channel_disable(rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
        }
        ret = i2s_del_channel(rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(ret));
            return ret;
        }
        rx_handle = NULL;
    }
    return ESP_OK;
}

int mic_get_sample_rate(void) {
    return mic_cfg.sample_rate;
}

size_t mic_get_buffer_size(void) {
    return mic_cfg.buffer_size;
}