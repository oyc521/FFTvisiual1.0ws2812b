#include "dual_core_com.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DUAL_CORE";

// 全局变量
static QueueHandle_t command_queue = NULL;
static core_status_t current_status;
static SemaphoreHandle_t status_mutex = NULL;
static QueueHandle_t audio_data_queue = NULL;

// 调试统计变量
/*
typedef struct {
    uint32_t audio_send_success;     // 音频数据发送成功次数
    uint32_t audio_send_dropped;     // 音频数据丢弃次数
    uint32_t audio_receive_success;  // 音频数据接收成功次数
    uint32_t audio_receive_timeout;  // 音频数据接收超时次数
    uint32_t command_send_success;   // 命令发送成功次数
    uint32_t command_receive_success;// 命令接收成功次数
} dual_core_stats_t;

static dual_core_stats_t debug_stats = {0};
*/

esp_err_t dual_core_com_init(void)
{
    ESP_LOGI(TAG, "初始化双核通信...");
    
    // 创建命令队列（长度10）
    command_queue = xQueueCreate(10, sizeof(core_command_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "创建命令队列失败");
        return ESP_FAIL;
    }
    
    // 创建音频数据队列（长度5，避免积压）
    audio_data_queue = xQueueCreate(5, sizeof(audio_data_t));
    if (audio_data_queue == NULL) {
        ESP_LOGE(TAG, "创建音频数据队列失败");
        vQueueDelete(command_queue);
        command_queue = NULL;
        return ESP_FAIL;
    }

    // 创建状态互斥锁
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        vQueueDelete(command_queue);
        vQueueDelete(audio_data_queue);
        command_queue = NULL;
        audio_data_queue = NULL;
        return ESP_FAIL;
    }
    
    // 初始化状态
    memset(&current_status, 0, sizeof(core_status_t));
    current_status.current_mode = MODE_SPECTRUM;
    current_status.wifi_connected = false;
    current_status.led_frame_count = 0;
    current_status.free_heap_size = esp_get_free_heap_size();
    
    // 重置调试统计
    /*
    memset(&debug_stats, 0, sizeof(dual_core_stats_t));
    */
    
    ESP_LOGI(TAG, "双核通信初始化完成");
    ESP_LOGI(TAG, "  命令队列容量: 10");
    ESP_LOGI(TAG, "  音频队列容量: 5");
    return ESP_OK;
}

esp_err_t dual_core_com_send_command(core_command_t *cmd, TickType_t timeout)
{
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "命令队列未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (cmd == NULL) {
        ESP_LOGE(TAG, "命令数据为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    BaseType_t result = xQueueSend(command_queue, cmd, timeout);
    if (result != pdPASS) {
        ESP_LOGW(TAG, "发送命令失败，队列可能已满");
        return ESP_FAIL;
    }
    
    /*
    debug_stats.command_send_success++;
    */
    
    ESP_LOGD(TAG, "命令发送成功，类型: %d", cmd->type);
    return ESP_OK;
}

esp_err_t dual_core_com_receive_command(core_command_t *cmd, TickType_t timeout)
{
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "命令队列未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (cmd == NULL) {
        ESP_LOGE(TAG, "命令数据指针为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    BaseType_t result = xQueueReceive(command_queue, cmd, timeout);
    if (result != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    
    /*
    debug_stats.command_receive_success++;
    */
    
    ESP_LOGD(TAG, "接收到命令，类型: %d", cmd->type);
    return ESP_OK;
}

void dual_core_com_get_status(core_status_t *status)
{
    if (status_mutex != NULL && xSemaphoreTake(status_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(status, &current_status, sizeof(core_status_t));
        xSemaphoreGive(status_mutex);
    }
}

void dual_core_com_update_status(led_mode_t new_mode, uint32_t frame_count)
{
    if (status_mutex != NULL && xSemaphoreTake(status_mutex, portMAX_DELAY) == pdTRUE) {
        if (new_mode >= MODE_SPECTRUM && new_mode <= MODE_OFF) {
            current_status.current_mode = new_mode;
        }
        
        if (frame_count > 0) {
            current_status.led_frame_count = frame_count;
        }
        
        current_status.free_heap_size = esp_get_free_heap_size();
        xSemaphoreGive(status_mutex);
    }
}

void dual_core_com_set_wifi_status(bool connected)
{
    if (status_mutex != NULL && xSemaphoreTake(status_mutex, portMAX_DELAY) == pdTRUE) {
        current_status.wifi_connected = connected;
        xSemaphoreGive(status_mutex);
    }
}

// 修改 dual_core_com_send_audio_data 函数
esp_err_t dual_core_com_send_audio_data(const float *bands, int num_bands, float total_energy) {
    if (audio_data_queue == NULL || bands == NULL || num_bands != NUM_FREQ_BANDS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    audio_data_t audio_data;
    memcpy(audio_data.frequency_bands, bands, NUM_FREQ_BANDS * sizeof(float));
    audio_data.total_energy = total_energy;
    audio_data.timestamp = xTaskGetTickCount();
    
    // 直接发送，如果队列满则丢弃当前数据（不尝试接收再发送）
    BaseType_t result = xQueueSend(audio_data_queue, &audio_data, 0);
    if (result != pdPASS) {
        // 队列满，丢弃当前数据包（不发送）
        return ESP_OK; // 返回成功，但数据被丢弃
    }
    
    return ESP_OK;
}

esp_err_t dual_core_com_receive_audio_data(audio_data_t *audio_data, TickType_t timeout)
{
    if (audio_data_queue == NULL || audio_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    BaseType_t result = xQueueReceive(audio_data_queue, audio_data, timeout);
    if (result != pdPASS) {
        /*
        debug_stats.audio_receive_timeout++;
        */
        return ESP_ERR_TIMEOUT;
    }
    
    /*
    debug_stats.audio_receive_success++;
    
    // 计算数据延迟
    uint32_t current_time = xTaskGetTickCount();
    uint32_t delay = current_time - audio_data->timestamp;
    
    // 定期报告延迟
    static uint32_t delay_report_count = 0;
    if (delay > 5 && delay_report_count++ % 50 == 0) {
        ESP_LOGW(TAG, "音频数据延迟较高: %lu ticks", delay);
    }
    */
    
    return ESP_OK;
}

// ==================== 调试监控函数（已注释，后续启用） ====================

/*
// 获取音频队列状态
UBaseType_t dual_core_com_get_audio_queue_count(void) {
    if (audio_data_queue == NULL) return 0;
    return uxQueueMessagesWaiting(audio_data_queue);
}

UBaseType_t dual_core_com_get_audio_queue_space(void) {
    if (audio_data_queue == NULL) return 0;
    return uxQueueSpacesAvailable(audio_data_queue);
}

// 获取命令队列状态
UBaseType_t dual_core_com_get_command_queue_count(void) {
    if (command_queue == NULL) return 0;
    return uxQueueMessagesWaiting(command_queue);
}

UBaseType_t dual_core_com_get_command_queue_space(void) {
    if (command_queue == NULL) return 0;
    return uxQueueSpacesAvailable(command_queue);
}

// 清空队列（用于调试）
void dual_core_com_clear_audio_queue(void) {
    if (audio_data_queue == NULL) return;
    
    audio_data_t dummy;
    while (xQueueReceive(audio_data_queue, &dummy, 0) == pdPASS) {
        // 清空队列
    }
}

void dual_core_com_clear_command_queue(void) {
    if (command_queue == NULL) return;
    
    core_command_t dummy;
    while (xQueueReceive(command_queue, &dummy, 0) == pdPASS) {
        // 清空队列
    }
}

// 获取调试统计信息
void dual_core_com_get_debug_stats(dual_core_stats_t *stats) {
    if (stats != NULL) {
        memcpy(stats, &debug_stats, sizeof(dual_core_stats_t));
    }
}

// 重置调试统计
void dual_core_com_reset_debug_stats(void) {
    memset(&debug_stats, 0, sizeof(dual_core_stats_t));
}

// 打印队列状态信息
void dual_core_com_print_status(void) {
    ESP_LOGI(TAG, "========== 双核通信状态 ==========");
    
    // 音频队列状态
    if (audio_data_queue != NULL) {
        UBaseType_t audio_items = uxQueueMessagesWaiting(audio_data_queue);
        UBaseType_t audio_spaces = uxQueueSpacesAvailable(audio_data_queue);
        ESP_LOGI(TAG, "音频队列: %lu/%lu (使用/总数)", audio_items, audio_items + audio_spaces);
    } else {
        ESP_LOGI(TAG, "音频队列: 未初始化");
    }
    
    // 命令队列状态
    if (command_queue != NULL) {
        UBaseType_t command_items = uxQueueMessagesWaiting(command_queue);
        UBaseType_t command_spaces = uxQueueSpacesAvailable(command_queue);
        ESP_LOGI(TAG, "命令队列: %lu/%lu (使用/总数)", command_items, command_items + command_spaces);
    } else {
        ESP_LOGI(TAG, "命令队列: 未初始化");
    }
    
    // 系统状态
    core_status_t status;
    dual_core_com_get_status(&status);
    ESP_LOGI(TAG, "当前模式: %d", status.current_mode);
    ESP_LOGI(TAG, "WiFi连接: %s", status.wifi_connected ? "是" : "否");
    ESP_LOGI(TAG, "LED帧数: %lu", status.led_frame_count);
    
    ESP_LOGI(TAG, "===================================");
}
*/

// ==================== 清理函数 ====================

// 清理双核通信资源
esp_err_t dual_core_com_deinit(void) {
    ESP_LOGI(TAG, "清理双核通信资源");
    
    // 先打印当前状态
    /*
    dual_core_com_print_status();
    */
    
    // 删除命令队列
    if (command_queue != NULL) {
        vQueueDelete(command_queue);
        command_queue = NULL;
    }
    
    // 删除音频数据队列
    if (audio_data_queue != NULL) {
        vQueueDelete(audio_data_queue);
        audio_data_queue = NULL;
    }
    
    // 删除互斥锁
    if (status_mutex != NULL) {
        vSemaphoreDelete(status_mutex);
        status_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "双核通信资源已清理");
    return ESP_OK;
}