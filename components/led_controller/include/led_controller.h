#ifndef __LED_CONTROLLER_H__
#define __LED_CONTROLLER_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 颜色结构体
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

// LED控制器配置
typedef struct {
    int gpio_pin;
    int num_leds;
    int brightness;  // 0-255
} led_config_t;

// 可视化模式
typedef enum {
    MODE_SPECTRUM = 0,      // 频谱显示
    MODE_RAINBOW = 1,       // 彩虹效果
    MODE_STARFIELD = 2,     // 星空（星辰漂移+鼓点超空间）
    MODE_METEOR_PULSE = 3,  // 流星脉冲
    MODE_WATER_RIPPLE = 4,  // 水波纹效果
    MODE_ENERGY_WAVE = 5,   // 能量波效果
    MODE_FIREWORKS = 6,     // 烟花效果
    MODE_RHYTHM_PULSE = 7,  // 节奏脉冲
    MODE_RHYTHM_BREATH = 8, // 节奏闪烁/呼吸模式
    MODE_RHYTHM_JUMP = 9,   // 节奏跳动效果
    MODE_SPARKLE_RAINBOW = 10, // 闪烁彩虹流水效果
    MODE_EXPLOSION = 11,    // 爆炸碰撞效果
    MODE_PEAK_HOLD = 12,    // 波峰余晖效果
    MODE_OFF = 13,          // 关闭
    MODE_MIRROR = 14,       // 中心对称频谱 / VU
    MODE_SHOCKWAVE = 15,    // 节拍冲击波
    MODE_AURORA = 16,       // 极光辉光（氛围流动）
    MODE_HEARTBEAT = 17,    // 心跳闪动（鼓点驱动）
    MODE_COUNT              // 模式总数（上界，不可选）
} led_mode_t;

// 统一节拍/音频特征（每帧由 led_update_visualization 计算一次，供所有效果共享）
typedef struct {
    float bass;     // 低频平均能量
    float mid;      // 中频平均能量
    float high;     // 高频平均能量
    float energy;   // 全带平均能量
    float flux;     // 频谱通量（正变化，onset 常用）
    float pulse;    // 节拍脉冲包络 0..1（onset 触发后衰减）
    uint8_t bpm;    // 估计 BPM（0 表示未知）
    bool onset;     // 本帧是否检测到鼓点
} led_beat_t;

// 统一效果参数（全局风格，所有模式共享）
typedef struct {
    float speed;        // 0.2-3.0 动画时间倍率
    float intensity;    // 0.2-2.0 幅度/亮度增益
    float sensitivity;  // 0.2-4.0 音频->视觉映射增益
    float hue;          // 0.0-1.0 全局色相偏移
    float color_speed;  // 0.0-4.0 颜色流动速度
    float beat_react;   // 0.0-1.0 鼓点联动强度
} led_fx_t;

// 设置全局效果参数
esp_err_t led_set_fx(const led_fx_t *fx);

// 读取全局效果参数
const led_fx_t *led_get_fx(void);

// 读取归一化后的 32 段显示频谱（每段 0-255），供网页实时频谱显示
void led_get_spectrum(uint8_t *out, int n);

// 获取当前节拍/音频特征（只读，供效果内使用）
const led_beat_t *led_get_beat(void);

// 设置全局后处理参数：gamma(>=1 提对比)、noise_gate(0-255 压黑底)、afterimage(0-0.9 余晖拖尾)
void led_set_post_params(float gamma, uint8_t noise_gate, float afterimage);

// 读取全局后处理参数
void led_get_post_params(float *gamma, uint8_t *gate, float *afterimage);

// 初始化LED控制器
esp_err_t led_controller_init(const led_config_t *config);

// 设置所有LED颜色
esp_err_t led_set_all(rgb_color_t color);

// 设置单个LED颜色
esp_err_t led_set_pixel(int index, rgb_color_t color);

// 清空所有LED
esp_err_t led_clear_all(void);

// 刷新显示（将颜色数据发送到LED）
esp_err_t led_show(void);

// 设置可视化模式
esp_err_t led_set_mode(led_mode_t mode);

// 设置全局亮度（0-100，百分比）
esp_err_t led_set_brightness(uint8_t brightness_percent);

// 获取当前全局亮度（0-100，百分比）
uint8_t led_get_brightness(void);

// 更新可视化效果
esp_err_t led_update_visualization(float *energy_bands, int num_bands);

// 测试函数：彩虹效果
esp_err_t led_test_rainbow(void);

#endif // __LED_CONTROLLER_H__