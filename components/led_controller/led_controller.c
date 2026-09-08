#include "led_controller.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "math.h"
#ifndef NUM_FREQ_BANDS
#define NUM_FREQ_BANDS 32  // 假设默认值为 32，根据实际情况调整
#endif
static const char *TAG = "LED_CTRL";

// 全局变量
static led_strip_handle_t strip = NULL;
static int led_count = 0;
static led_mode_t current_mode = MODE_OFF;
static uint32_t animation_counter = 0;

static uint8_t s_brightness_percent = 100;
static float s_global_brightness = 1.0f;

static inline uint8_t scale_channel(uint8_t ch)
{
    float v = (float)ch * s_global_brightness;
    if (v > 255.0f) v = 255.0f;
    if (v < 0.0f) v = 0.0f;
    return (uint8_t)v;
}

static inline rgb_color_t scale_color(rgb_color_t c)
{
    c.r = scale_channel(c.r);
    c.g = scale_channel(c.g);
    c.b = scale_channel(c.b);
    return c;
}

// ===================== 统一效果参数（全局风格） =====================
static led_beat_t s_beat;
static led_fx_t g_fx = {
    .speed = 1.0f,
    .intensity = 1.0f,
    .sensitivity = 1.0f,
    .hue = 0.0f,
    .color_speed = 1.0f,
    .beat_react = 0.6f,
};

static float fx_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

esp_err_t led_set_fx(const led_fx_t *fx)
{
    if (!fx) return ESP_ERR_INVALID_ARG;
    g_fx.speed       = fx_clampf(fx->speed, 0.2f, 3.0f);
    g_fx.intensity   = fx_clampf(fx->intensity, 0.2f, 2.0f);
    g_fx.sensitivity = fx_clampf(fx->sensitivity, 0.2f, 4.0f);
    float h = fx->hue;
    h -= floorf(h);
    g_fx.hue = h;
    g_fx.color_speed = fx_clampf(fx->color_speed, 0.0f, 4.0f);
    g_fx.beat_react  = fx_clampf(fx->beat_react, 0.0f, 1.0f);
    return ESP_OK;
}

const led_fx_t *led_get_fx(void) { return &g_fx; }

static inline float fx_sens(float x)   { return x * g_fx.sensitivity; }
static inline float fx_inten(float v)  { return v * g_fx.intensity; }
static inline float fx_hue(float h)    { h += g_fx.hue; h -= floorf(h); return h; }
static inline float fx_beat(void)      { return s_beat.pulse * g_fx.beat_react; }

// ===================== 全局后处理管线 =====================
static float    g_post_gamma     = 1.18f;
static uint8_t  g_post_gate      = 8;
static float    g_post_afterglow = 0.62f;

typedef struct { uint8_t r, g, b; } post_px_t;
static post_px_t s_post_prev[256];

static esp_err_t led_out_pixel(int idx, rgb_color_t c)
{
    if (!strip || idx < 0 || idx >= led_count) return ESP_ERR_INVALID_ARG;

    uint8_t dr = c.r, dg = c.g, db = c.b;

    if (led_count <= 256) {
        post_px_t *p = &s_post_prev[idx];
        uint8_t pr = (uint8_t)(p->r * g_post_afterglow);
        uint8_t pg = (uint8_t)(p->g * g_post_afterglow);
        uint8_t pb = (uint8_t)(p->b * g_post_afterglow);
        if (pr > dr) dr = pr;
        if (pg > dg) dg = pg;
        if (pb > db) db = pb;
        p->r = dr;
        p->g = dg;
        p->b = db;
    }

    if (dr < g_post_gate && dg < g_post_gate && db < g_post_gate) {
        dr = dg = db = 0;
    }

    const float inv255 = 1.0f / 255.0f;
    float rr = powf(fx_inten(dr * inv255), g_post_gamma) * 255.0f * s_global_brightness;
    float rg = powf(fx_inten(dg * inv255), g_post_gamma) * 255.0f * s_global_brightness;
    float rb = powf(fx_inten(db * inv255), g_post_gamma) * 255.0f * s_global_brightness;
    if (rr > 255.0f) rr = 255.0f;
    if (rg > 255.0f) rg = 255.0f;
    if (rb > 255.0f) rb = 255.0f;

    return led_strip_set_pixel(strip, idx, (uint8_t)rr, (uint8_t)rg, (uint8_t)rb);
}

void led_set_post_params(float gamma, uint8_t noise_gate, float afterimage)
{
    if (gamma < 1.0f) gamma = 1.0f;
    if (gamma > 2.5f) gamma = 2.5f;
    if (afterimage < 0.0f) afterimage = 0.0f;
    if (afterimage > 0.9f) afterimage = 0.9f;
    g_post_gamma = gamma;
    g_post_gate = noise_gate;
    g_post_afterglow = afterimage;
}

void led_get_post_params(float *gamma, uint8_t *gate, float *afterimage)
{
    if (gamma) *gamma = g_post_gamma;
    if (gate) *gate = g_post_gate;
    if (afterimage) *afterimage = g_post_afterglow;
}

// ===================== 统一节拍引擎 =====================
static float s_prev_bands[NUM_FREQ_BANDS];
static float s_bass_hist = 0.0f;
static float s_flux_hist = 0.0f;
static uint32_t s_last_beat_ms = 0;
static float s_beat_interval_ms = 0.0f;

const led_beat_t *led_get_beat(void)
{
    return &s_beat;
}

static void led_beat_update(const float *bands, int n)
{
    if (!bands || n <= 0) {
        s_beat.pulse *= 0.86f;
        if (s_beat.pulse < 0.02f) s_beat.pulse = 0.0f;
        s_beat.onset = false;
        return;
    }
    if (n > NUM_FREQ_BANDS) n = NUM_FREQ_BANDS;

    float bass = 0, mid = 0, high = 0, tot = 0, flux = 0;
    int nb = 0, nm = 0, nh = 0;
    int b_end = n / 4;
    int m_end = n * 4 / 5;

    for (int i = 0; i < n; i++) {
        float v = bands[i];
        tot += v;
        if (i < b_end) { bass += v; nb++; }
        else if (i < m_end) { mid += v; nm++; }
        else { high += v; nh++; }
        float d = v - s_prev_bands[i];
        if (d > 0) flux += d;
        s_prev_bands[i] = v;
    }
    if (nb) bass /= nb;
    if (nm) mid /= nm;
    if (nh) high /= nh;
    tot /= n;

    s_bass_hist = s_bass_hist * 0.90f + bass * 0.10f;
    s_flux_hist = s_flux_hist * 0.90f + flux * 0.10f;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t since = now_ms - s_last_beat_ms;
    bool onset = false;
    if (since > 250) {
        if ((bass > s_bass_hist * 1.28f && bass > 20.0f) ||
            (flux > s_flux_hist * 1.7f && flux > 25.0f)) {
            onset = true;
        }
    }

    s_beat.bass = bass;
    s_beat.mid = mid;
    s_beat.high = high;
    s_beat.energy = tot;
    s_beat.flux = flux;
    s_beat.onset = onset;

    if (onset) {
        if (s_last_beat_ms != 0 && since > 250 && since < 2000) {
            if (s_beat_interval_ms <= 0) s_beat_interval_ms = since;
            else s_beat_interval_ms = s_beat_interval_ms * 0.7f + since * 0.3f;
            float bpm = 60000.0f / s_beat_interval_ms;
            if (bpm < 30) bpm = 30;
            if (bpm > 240) bpm = 240;
            s_beat.bpm = (uint8_t)(bpm + 0.5f);
        }
        s_last_beat_ms = now_ms;
        s_beat.pulse = 1.0f;
    } else {
        s_beat.pulse *= 0.86f;
        if (s_beat.pulse < 0.02f) s_beat.pulse = 0.0f;
    }
}

// 显示用 32 段归一化频谱（供网页实时频谱显示）
static uint8_t s_disp_bands[NUM_FREQ_BANDS];
static float s_disp_gain = 1.0f;

static void led_update_display_spectrum(const float *bands, int n)
{
    if (!bands || n <= 0) return;
    if (n > NUM_FREQ_BANDS) n = NUM_FREQ_BANDS;
    float mx = 1.0f;
    for (int i = 0; i < n; i++) if (bands[i] > mx) mx = bands[i];
    s_disp_gain = s_disp_gain * 0.95f;
    if (mx > s_disp_gain) s_disp_gain = mx;
    if (s_disp_gain < 1.0f) s_disp_gain = 1.0f;
    for (int i = 0; i < n; i++) {
        float v = bands[i] / s_disp_gain;
        if (v > 1.0f) v = 1.0f;
        if (v < 0.0f) v = 0.0f;
        s_disp_bands[i] = (uint8_t)(v * 255.0f);
    }
}

void led_get_spectrum(uint8_t *out, int n)
{
    if (!out) return;
    if (n > NUM_FREQ_BANDS) n = NUM_FREQ_BANDS;
    for (int i = 0; i < n; i++) out[i] = s_disp_bands[i];
}

// 流星脉冲效果专用变量
typedef struct {
    float position;           // 当前流星位置
    int direction;            // 移动方向：1=从左到右，-1=从右到左
    float speed;              // 移动速度
    bool active;              // 是否活跃
    int length;               // 流星长度（动态）
    float brightness;         // 当前亮度
    float max_brightness;     // 最大亮度
    float fade_speed;         // 衰减速度
    rgb_color_t head_color;   // 流星头部颜色
    rgb_color_t tail_color;   // 流星尾部颜色
    float age;                // 年龄（用于计算生命周期）
    float lifespan;           // 寿命
    float trail_width;        // 尾迹宽度
    bool has_sparkle;         // 是否带有闪烁效果
    float sparkle_intensity;  // 闪烁强度
} meteor_pulse_t;

// 流星脉冲系统状态
#define MAX_METEORS 4  // 最大同时存在的流星数量

static struct {
    meteor_pulse_t meteors[MAX_METEORS];  // 流星数组
    
    uint32_t last_trigger_time;           // 上次触发时间
    float min_interval;                   // 最小触发间隔
    float max_interval;                   // 最大触发间隔
    
    float min_speed;                      // 最小速度
    float max_speed;                      // 最大速度
    int min_length;                       // 最小长度
    int max_length;                       // 最大长度
    
    float energy_history[10];             // 能量历史记录（用于检测节奏）
    int energy_history_index;             // 能量历史索引
    
    float color_hue;                      // 基础色调
    float hue_speed;                      // 色调变化速度
    
    uint32_t frame_counter;               // 帧计数器
    
} meteor_pulse_state = {
    .last_trigger_time = 0,
    .min_interval = 20.0f,     // 最小10帧间隔
    .max_interval = 85.0f,     // 最大60帧间隔
    .min_speed = 0.2f,
    .max_speed = 0.8f,
    .min_length = 3,
    .max_length = 5,
    .energy_history_index = 0,
    .color_hue = 0.0f,
    .hue_speed = 0.0015f,
    .frame_counter = 0
};

// 烟花效果专用变量
static struct {
    // 烟花粒子
    struct {
        float position;
        float velocity;
        float brightness;
        float size;        // 粒子大小
        uint32_t lifetime;
        rgb_color_t color;
        bool active;
    } particles[30];  // 增加到30个粒子
    
    // 烟花爆炸点
    struct {
        float position;
        float intensity;
        float radius;      // 爆炸半径（LED数量）
        uint32_t start_time;
        rgb_color_t color;
        bool active;
    } explosions[5];  // 同时存在的爆炸点
    
    uint32_t last_firework_time;
    float energy_history[5];
    int history_index;
    
} fireworks_state = {
    .last_firework_time = 0,
    .history_index = 0
};

// 脉冲效果专用变量
typedef struct {
    int direction;          // 0=从左向右，1=从右向左
    float position;         // 当前位置
    float speed;            // 移动速度
    int length;             // 脉冲长度
    float intensity;        // 脉冲强度
    rgb_color_t color;      // 脉冲颜色
    bool active;            // 是否活跃
    uint32_t start_time;    // 开始时间
} pulse_t;

static pulse_t pulses[10];  // 最多10个同时存在的脉冲

// 全局效果状态（主要保留颜色缓冲区）
static struct {
    // 颜色缓冲区，用于叠加效果
    rgb_color_t *color_buffer;
} effects_state = {
    .color_buffer = NULL
};

// 水波纹效果专用变量（增强版动态水波纹）
static struct {
    // 水波纹数组
    struct {
        float position;     // 位置
        float intensity;    // 强度
        float size;         // 当前大小
        float speed;        // 传播速度
        uint32_t start_time; // 开始时间
        rgb_color_t color;  // 颜色
        bool active;        // 是否活跃
        int type;           // 0=低频大水波纹, 1=中频中水波纹, 2=高频小水波纹
    } ripples[10];          // 最多10个水波纹同时存在
    
    // 能量历史
    float energy_history[3]; // 低、中、高频能量历史
    uint32_t last_update_time;
    
    // 背景波动
    float background_phase;
    float background_amplitude;
} water_ripple_state;

// 能量波效果专用变量（增强版动态频谱）
static struct {
    // 频谱条带能量（平滑处理后的）
    float smoothed_energies[24]; // 最多24个条带
    // 峰值保持
    float peak_energies[24];
    uint32_t peak_timers[24];
    
    // 动态效果
    float pulse_wave;       // 脉冲波动
    float wave_offset;      // 波动偏移
    float hue_shift;        // 色调偏移
    uint32_t last_beat_time; // 上次节拍时间
} energy_wave_state;

// 节奏闪烁模式专用变量
static struct {
    float breath_phase;          // 呼吸相位 (0-2π)
    float breath_speed;          // 呼吸速度
    float breath_min;            // 最小亮度
    float breath_max;            // 最大亮度
    
    float color_hue;             // 当前色调 (0-1)
    float hue_speed;             // 色调变化速度
    
    float bass_energy_history[5]; // 低频能量历史（用于节拍检测）
    int energy_history_index;
    float beat_threshold;        // 节拍检测阈值
    uint32_t last_beat_time;     // 上次检测到节拍的时间
    
    float flash_intensity;       // 当前闪烁强度 (0-1)
    float flash_decay_rate;      // 闪烁衰减率
    rgb_color_t flash_color;     // 闪烁颜色
    
} rhythm_breath_state = {
    .breath_phase = 0,
    .breath_speed = 0.05f,
    .breath_min = 0.03f,
    .breath_max = 1.0f,
    .color_hue = 0,
    .hue_speed = 0.001f,
    .beat_threshold = 20.0f,
    .last_beat_time = 0,
    .flash_intensity = 0,
    .flash_decay_rate = 0.9f,
    .flash_color = {255, 255, 255}
};
// 节奏跳动效果专用变量
static struct {
    struct {
        float height;        // 当前高度 (0-1)
        float velocity;      // 当前速度
        float target_height; // 目标高度
        float mass;          // 质量
        float stiffness;     // 刚度（弹性系数）
        float damping;       // 阻尼系数
        rgb_color_t color;   // 该段颜色
    } segments[8];           // 分成8个跳动段
    
    float color_hue;
    float hue_speed;
    
    float energy_history[8];
    float energy_peak[8];    // 能量峰值，用于更动态的响应
    float peak_decay;        // 峰值衰减速度
    
    float response_speed;    // 响应速度
    float gravity;           // 重力影响
    float brightness_boost;  // 亮度增强
    
} rhythm_jump_state = {
    .color_hue = 0,
    .hue_speed = 0.003f,     // 增加颜色变化速度
    .response_speed = 0.5f,  // 增加响应速度
    .gravity = 0.85f,        // 减小重力，让跳动更高
    .brightness_boost = 1.5f, // 增加亮度增强系数
    .peak_decay = 0.95f      // 峰值衰减
};


// 闪烁彩虹效果专用变量
typedef struct {
    float position;      // 当前位置（LED索引）
    float speed;         // 移动速度（LED/帧）
    int size;           // 颗粒大小（1-5个LED）
    rgb_color_t color;   // 颗粒颜色
    float brightness;    // 当前亮度（0-1）
    float flash_speed;   // 闪烁速度
    int direction;       // 移动方向：1=正向，-1=反向
    int lifetime;        // 生命周期（帧数）
    bool active;         // 是否活跃
} sparkle_t;

static struct {
    float rainbow_offset;   // 彩虹偏移量
    float rainbow_speed;    // 彩虹流动速度
    
    sparkle_t sparkles[15]; // 最多15个同时存在的颗粒
    
    uint32_t last_emit_time; // 上次发射时间
    float emit_interval;     // 发射间隔（帧数）
    int emit_direction;      // 发射方向（1=正向，-1=反向，0=随机）
    int next_direction;      // 下次发射方向（用于交替）
    
    float energy_threshold;  // 发射能量阈值
    float last_energy;       // 上次能量值
    
    float hue_base;          // 基础色调
    float hue_speed;         // 色调变化速度
    
} sparkle_rainbow_state = {
    .rainbow_offset = 0,
    .rainbow_speed = 0.05f,
    .last_emit_time = 0,
    .emit_interval = 30.0f,
    .emit_direction = 0,
    .next_direction = 1,
    .energy_threshold = 15.0f,
    .last_energy = 0,
    .hue_base = 0,
    .hue_speed = 0.001f
};

// 爆炸碰撞效果专用变量
typedef enum {
    STATE_IDLE,          // 空闲状态，等待发射
    STATE_ION_FLYING,    // 离子飞行中
    STATE_EXPLODING,     // 爆炸进行中
    STATE_PARTICLE_PHASE // 粒子附着阶段
} explosion_state_t;

typedef struct {
    float position;      // 当前位置（LED索引）
    float velocity;      // 速度（LED/帧）
    rgb_color_t color;   // 离子颜色
    float brightness;    // 当前亮度（0-1）
    float trail_length;  // 拖尾长度
    float size;          // 离子大小（影响光晕范围）
    bool active;         // 是否活跃
    int direction;       // 移动方向：1=向右，-1=向左
    float spawn_time;    // 发射时间（用于计算轨迹效果）
} ion_particle_t;

typedef struct {
    float position;      // 爆炸中心位置
    float radius;        // 当前爆炸半径
    float max_radius;    // 最大爆炸半径
    float intensity;     // 爆炸强度（0-1）
    float decay_rate;    // 衰减率
    rgb_color_t core_color;     // 核心颜色
    rgb_color_t shockwave_color; // 冲击波颜色
    uint32_t start_time; // 开始时间（帧数）
    uint32_t duration;   // 持续时间（帧数）
    bool active;         // 是否活跃
} explosion_t;

typedef struct {
    float position;      // 粒子位置
    float velocity;      // 粒子速度
    float acceleration;  // 加速度
    rgb_color_t color;   // 粒子颜色
    float brightness;    // 当前亮度
    float max_brightness; // 最大亮度
    float brightness_speed; // 亮度变化速度
    float flicker_speed; // 闪烁速度
    float flicker_phase; // 闪烁相位
    float size;          // 粒子大小
    float lifespan;      // 寿命（帧数）
    float age;           // 当前年龄（帧数）
    bool active;         // 是否活跃
    int direction;       // 运动方向：1=向右，-1=向左，0=随机
    float gravity;       // 重力影响
    float friction;      // 摩擦力
} explosion_particle_t;

#define MAX_PARTICLES 60  // 最大粒子数

static struct {
    explosion_state_t state;
    uint32_t state_timer;       // 状态计时器
    
    ion_particle_t left_ion;    // 左端离子
    ion_particle_t right_ion;   // 右端离子
    
    explosion_t explosion;
    
    explosion_particle_t particles[MAX_PARTICLES];
    
    uint32_t last_launch_time;  // 上次发射时间
    float launch_interval;      // 发射间隔（最小帧数）
    float energy_threshold;     // 发射能量阈值
    float last_energy;          // 上次能量值
    
    float hue_left;             // 左端离子色调
    float hue_right;            // 右端离子色调
    float hue_speed;            // 色调变化速度
    
    float collision_zone_start; // 碰撞区域起始（LED比例）
    float collision_zone_end;   // 碰撞区域结束（LED比例）
    
    uint32_t collision_count;   // 碰撞次数统计
    uint32_t particle_timer;    // 粒子更新计时器
    
} explosion_collision_state = {
    .state = STATE_IDLE,
    .state_timer = 0,
    .last_launch_time = 0,
    .launch_interval = 30.0f,     // 缩短发射间隔
    .energy_threshold = 20.0f,    // 降低能量阈值
    .last_energy = 0,
    .hue_left = 0.0f,
    .hue_right = 0.6f,
    .hue_speed = 0.003f,          // 增加颜色变化速度
    .collision_zone_start = 0.4f,
    .collision_zone_end = 0.6f,
    .collision_count = 0,
    .particle_timer = 0
};

// 函数原型声明
static esp_err_t meteor_pulse_effect(float *energy_bands, int num_bands);
static rgb_color_t hsv_to_rgb(float h, float s, float v);
static esp_err_t water_ripple_effect(float *energy_bands, int num_bands);
static esp_err_t energy_wave_effect(float *energy_bands, int num_bands);
static esp_err_t fireworks_effect_improved(float *energy_bands, int num_bands);
static rgb_color_t get_firework_color(int type);
static esp_err_t rhythm_pulse_effect(float *energy_bands, int num_bands);
static esp_err_t rhythm_breath_effect(float *energy_bands, int num_bands);
static esp_err_t rhythm_jump_effect(float *energy_bands, int num_bands);
static rgb_color_t get_energy_color(float bass_energy, float mid_energy, float high_energy);
static esp_err_t sparkle_rainbow_effect(float *energy_bands, int num_bands);
static rgb_color_t get_ion_color_from_energy(float bass_energy, float mid_energy, float high_energy, int direction);
static rgb_color_t mix_ion_colors(rgb_color_t color_left, rgb_color_t color_right);
static esp_err_t explosion_collision_effect(float *energy_bands, int num_bands);
static void init_color_buffer(void);
static void clear_color_buffer(void);
static void set_buffer_pixel(int index, rgb_color_t color);
static void set_buffer_pixel_blend(int index, rgb_color_t color, float blend_factor);
static void trigger_firework_explosion(float position, float intensity, rgb_color_t color);
static void create_pulse(int direction, rgb_color_t color, int length, float speed, float intensity);
static void draw_meteor(meteor_pulse_t *meteor);
static void update_and_draw_pulses(void);
static void init_jump_segments(void);
static void update_physics_simulation(void);
static void init_sparkles(void);
static void create_sparkle(float position, int direction, rgb_color_t color, float speed, int size);
static void init_ion_particle(ion_particle_t *ion, int direction, rgb_color_t color);
static void init_explosion(float position, float intensity, rgb_color_t core_color);
static bool check_collision(void);
static float rgb_to_hue(rgb_color_t color);
//static void init_ion_particle(ion_particle_t *ion, int direction, rgb_color_t color); 
// 烟花颜色生成函数
static rgb_color_t get_firework_color(int type) {
    switch (type) {
        case 0: return (rgb_color_t){255, 50, 50};     // 红色
        case 1: return (rgb_color_t){255, 150, 30};    // 橙色
        case 2: return (rgb_color_t){255, 255, 50};    // 黄色
        case 3: return (rgb_color_t){50, 255, 50};     // 绿色
        case 4: return (rgb_color_t){50, 150, 255};    // 蓝色
        case 5: return (rgb_color_t){200, 50, 255};    // 紫色
        default: return (rgb_color_t){255, 100, 100};  // 默认粉色
    }
}

// 创建新脉冲
static void create_pulse(int direction, rgb_color_t color, int length, float speed, float intensity) {
    for (int i = 0; i < 10; i++) {
        if (!pulses[i].active) {
            pulses[i].active = true;
            pulses[i].direction = direction;
            pulses[i].color = color;
            pulses[i].length = length;
            pulses[i].speed = speed;
            pulses[i].intensity = fminf(intensity, 1.0f);
            pulses[i].start_time = animation_counter;
            
            // 设置起始位置
            if (direction == 0) { // 从左向右
                pulses[i].position = -length;
            } else { // 从右向左
                pulses[i].position = led_count;
            }
            
            break;
        }
    }
}

// 更新和绘制所有脉冲
static void update_and_draw_pulses(void) {
    for (int i = 0; i < 10; i++) {
        if (pulses[i].active) {
            // 更新位置
            if (pulses[i].direction == 0) { // 从左向右
                pulses[i].position += pulses[i].speed * g_fx.speed;
            } else { // 从右向左
                pulses[i].position -= pulses[i].speed * g_fx.speed;
            }
            
            // 计算脉冲年龄（用于淡出效果）
            uint32_t age = animation_counter - pulses[i].start_time;
            float age_factor = 1.0f - (float)age / 100.0f; // 100帧后完全消失
            if (age_factor < 0) age_factor = 0;
            
            // 绘制脉冲
            for (int l = 0; l < pulses[i].length; l++) {
                int led_pos;
                if (pulses[i].direction == 0) {
                    led_pos = (int)(pulses[i].position + l);
                } else {
                    led_pos = (int)(pulses[i].position - l);
                }
                
                if (led_pos >= 0 && led_pos < led_count) {
                    // 计算每个LED的强度（脉冲头部最亮，尾部渐暗）
                    float led_intensity;
                    if (pulses[i].direction == 0) {
                        // 从左向右：最右边的最亮
                        led_intensity = (float)(l + 1) / pulses[i].length;
                    } else {
                        // 从右向左：最左边的最亮
                        led_intensity = (float)(pulses[i].length - l) / pulses[i].length;
                    }
                    
                    // 应用年龄衰减和整体强度
                    float final_intensity = pulses[i].intensity * led_intensity * age_factor;
                    
                    if (final_intensity > 0.01f) {
                        rgb_color_t led_color = {
                            .r = (uint8_t)(pulses[i].color.r * final_intensity),
                            .g = (uint8_t)(pulses[i].color.g * final_intensity),
                            .b = (uint8_t)(pulses[i].color.b * final_intensity)
                        };
                        
                        // 混合到缓冲区
                        set_buffer_pixel_blend(led_pos, led_color, 1.0f);
                        
                        // 添加辉光效果
                        if (final_intensity > 0.3f) {
                            // 向前后各扩散1个LED
                            for (int d = 1; d <= 2; d++) {
                                float glow_intensity = final_intensity * (0.5f / d);
                                
                                rgb_color_t glow_color = {
                                    .r = (uint8_t)(pulses[i].color.r * glow_intensity),
                                    .g = (uint8_t)(pulses[i].color.g * glow_intensity),
                                    .b = (uint8_t)(pulses[i].color.b * glow_intensity)
                                };
                                
                                if (led_pos - d >= 0) {
                                    set_buffer_pixel_blend(led_pos - d, glow_color, 0.6f);
                                }
                                if (led_pos + d < led_count) {
                                    set_buffer_pixel_blend(led_pos + d, glow_color, 0.6f);
                                }
                            }
                        }
                    }
                }
            }
            
            // 检查脉冲是否完全离开屏幕
            bool out_of_bounds = false;
            if (pulses[i].direction == 0) {
                out_of_bounds = pulses[i].position > led_count;
            } else {
                out_of_bounds = pulses[i].position < -pulses[i].length;
            }
            
            if (out_of_bounds || age_factor <= 0) {
                pulses[i].active = false;
            }
        }
    }
}

// 初始化LED控制器
esp_err_t led_controller_init(const led_config_t *config) {
    if (!config || config->num_leds <= 0) {
        ESP_LOGE(TAG, "无效的配置参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "初始化LED控制器...");
    ESP_LOGI(TAG, "  GPIO引脚: %d", config->gpio_pin);
    ESP_LOGI(TAG, "  LED数量: %d", config->num_leds);
    
    // LED灯带配置
    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_pin,
        .max_leds = config->num_leds,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建LED灯带失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = led_strip_clear(strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "清空LED失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    led_count = config->num_leds;
    current_mode = MODE_OFF;

    uint8_t bp = (config->brightness > 100) ? 100 : config->brightness;
    s_brightness_percent = bp;
    s_global_brightness = (float)bp / 100.0f;

    init_color_buffer(); // 初始化颜色缓冲区

    // 初始化烟花状态
    for (int i = 0; i < 30; i++) {
        fireworks_state.particles[i].active = false;
    }
    for (int i = 0; i < 5; i++) {
        fireworks_state.explosions[i].active = false;
    }
    for (int i = 0; i < 5; i++) {
        fireworks_state.energy_history[i] = 0;
    }
    
    // 初始化流星状态

    
    // 初始化脉冲状态
    for (int i = 0; i < 10; i++) {
        pulses[i].active = false;
    }
    
    // 初始化水波纹状态
    for (int i = 0; i < 10; i++) {
        water_ripple_state.ripples[i].active = false;
    }
    for (int i = 0; i < 3; i++) {
        water_ripple_state.energy_history[i] = 0;
    }
    water_ripple_state.last_update_time = 0;
    water_ripple_state.background_phase = 0;
    water_ripple_state.background_amplitude = 0;
    
    // 初始化能量波状态
    for (int i = 0; i < 24; i++) {
        energy_wave_state.smoothed_energies[i] = 0;
        energy_wave_state.peak_energies[i] = 0;
        energy_wave_state.peak_timers[i] = 0;
    }
    energy_wave_state.pulse_wave = 0;
    energy_wave_state.wave_offset = 0;
    energy_wave_state.hue_shift = 0;
    energy_wave_state.last_beat_time = 0;
    
    ESP_LOGI(TAG, "LED控制器初始化成功");
    return ESP_OK;
}

// 初始化颜色缓冲区
static void init_color_buffer(void) {
    if (effects_state.color_buffer != NULL) {
        free(effects_state.color_buffer);
    }
    
    effects_state.color_buffer = (rgb_color_t *)malloc(sizeof(rgb_color_t) * led_count);
    if (effects_state.color_buffer == NULL) {
        ESP_LOGE(TAG, "颜色缓冲区内存分配失败");
        return;
    }
    clear_color_buffer();
}

// 清空颜色缓冲区
static void clear_color_buffer(void) {
    if (effects_state.color_buffer == NULL) return;
    
    for (int i = 0; i < led_count; i++) {
        effects_state.color_buffer[i].r = 0;
        effects_state.color_buffer[i].g = 0;
        effects_state.color_buffer[i].b = 0;
    }
}

// 设置缓冲区像素（直接覆盖）
static void set_buffer_pixel(int index, rgb_color_t color) {
    if (index < 0 || index >= led_count || effects_state.color_buffer == NULL) return;
    
    effects_state.color_buffer[index] = color;
}

// 设置缓冲区像素（混合模式，取最大值）
static void set_buffer_pixel_blend(int index, rgb_color_t color, float blend_factor) {
    if (index < 0 || index >= led_count || effects_state.color_buffer == NULL) return;
    
    if (blend_factor > 1.0f) blend_factor = 1.0f;
    if (blend_factor < 0.0f) blend_factor = 0.0f;
    
    rgb_color_t *current = &effects_state.color_buffer[index];
    
    // 取每个通道的最大值（加性混合）
    if (color.r * blend_factor > current->r) {
        current->r = (uint8_t)(color.r * blend_factor);
    }
    if (color.g * blend_factor > current->g) {
        current->g = (uint8_t)(color.g * blend_factor);
    }
    if (color.b * blend_factor > current->b) {
        current->b = (uint8_t)(color.b * blend_factor);
    }
}

// 将缓冲区内容应用到LED灯带（统一走后处理出口）
static esp_err_t apply_color_buffer(void) {
    if (!strip || effects_state.color_buffer == NULL) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < led_count; i++) {
        esp_err_t ret = led_out_pixel(i, effects_state.color_buffer[i]);
        if (ret != ESP_OK) return ret;
    }

    return ESP_OK;
}

// 设置所有LED颜色
esp_err_t led_set_all(rgb_color_t color) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    for (int i = 0; i < led_count; i++) {
        rgb_color_t out_color = scale_color(color);
        esp_err_t ret = led_strip_set_pixel(strip, i, out_color.r, out_color.g, out_color.b);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "设置LED %d 失败: %s", i, esp_err_to_name(ret));
            return ret;
        }
    }
    
    return led_strip_refresh(strip);
}

// 设置单个LED颜色
esp_err_t led_set_pixel(int index, rgb_color_t color) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= led_count) return ESP_ERR_INVALID_ARG;
    
    rgb_color_t out_color = scale_color(color);
    esp_err_t ret = led_strip_set_pixel(strip, index, out_color.r, out_color.g, out_color.b);
    if (ret != ESP_OK) return ret;
    
    return ESP_OK;
}

// 清空所有LED
esp_err_t led_clear_all(void) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    esp_err_t ret = led_strip_clear(strip);
    if (ret != ESP_OK) return ret;
    
    return led_strip_refresh(strip);
}

// 刷新显示
esp_err_t led_show(void) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    return led_strip_refresh(strip);
}

// HSV转RGB
static rgb_color_t hsv_to_rgb(float h, float s, float v) {
    rgb_color_t color;

    h = fx_hue(h);
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (s > 1.0f) s = 1.0f;
    if (s < 0.0f) s = 0.0f;

    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    
    switch (i % 6) {
        case 0: color.r = v * 255; color.g = t * 255; color.b = p * 255; break;
        case 1: color.r = q * 255; color.g = v * 255; color.b = p * 255; break;
        case 2: color.r = p * 255; color.g = v * 255; color.b = t * 255; break;
        case 3: color.r = p * 255; color.g = q * 255; color.b = v * 255; break;
        case 4: color.r = t * 255; color.g = p * 255; color.b = v * 255; break;
        case 5: color.r = v * 255; color.g = p * 255; color.b = q * 255; break;
        default: color.r = 0; color.g = 0; color.b = 0; break;
    }
    
    return color;
}

// 触发烟花爆炸
static void trigger_firework_explosion(float position, float intensity, rgb_color_t color) {
    // 寻找空闲的爆炸点
    for (int i = 0; i < 5; i++) {
        if (!fireworks_state.explosions[i].active) {
            fireworks_state.explosions[i].active = true;
            fireworks_state.explosions[i].position = position;
            fireworks_state.explosions[i].intensity = intensity;
            fireworks_state.explosions[i].radius = 2.0f + intensity * 4.0f; // 2-6个LED的爆炸半径
            fireworks_state.explosions[i].start_time = animation_counter;
            fireworks_state.explosions[i].color = color;
            
            // 创建爆炸粒子
            int particle_count = (int)(8 + intensity * 8); // 8-16个粒子
            particle_count = particle_count > 30 ? 30 : particle_count;
            
            for (int p = 0; p < particle_count; p++) {
                // 寻找空闲粒子
                for (int j = 0; j < 30; j++) {
                    if (!fireworks_state.particles[j].active) {
                        fireworks_state.particles[j].active = true;
                        fireworks_state.particles[j].position = position;
                        
                        // 随机角度和速度
                        float angle = ((float)rand() / RAND_MAX) * 2 * M_PI;
                        float speed = 0.2f + ((float)rand() / RAND_MAX) * 0.8f * intensity;
                        fireworks_state.particles[j].velocity = cosf(angle) * speed;
                        
                        fireworks_state.particles[j].brightness = 1.0f;
                        fireworks_state.particles[j].size = 0.5f + ((float)rand() / RAND_MAX) * 0.5f;
                        fireworks_state.particles[j].lifetime = 50 + (rand() % 50);
                        
                        // 粒子颜色：基于爆炸颜色稍有变化
                        float color_variation = 0.8f + ((float)rand() / RAND_MAX) * 0.4f; // 0.8-1.2
                        fireworks_state.particles[j].color.r = 
                            (uint8_t)fmaxf(0, fminf(255, color.r * color_variation));
                        fireworks_state.particles[j].color.g = 
                            (uint8_t)fmaxf(0, fminf(255, color.g * color_variation));
                        fireworks_state.particles[j].color.b = 
                            (uint8_t)fmaxf(0, fminf(255, color.b * color_variation));
                        
                        break;
                    }
                }
            }
            
            ESP_LOGI(TAG, "烟花爆炸在位置: %.1f, 强度: %.2f, 半径: %.1f, 粒子数: %d",
                    position, intensity, fireworks_state.explosions[i].radius, particle_count);
            break;
        }
    }
}

// 测试彩虹效果
esp_err_t led_test_rainbow(void) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "开始彩虹测试");
    
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < led_count; i++) {
            float hue = (float)i / led_count + (float)cycle * 0.1;
            hue = hue - (int)hue;
            
            rgb_color_t color = hsv_to_rgb(hue, 1.0, 0.5);
            rgb_color_t out_color = scale_color(color);
            led_strip_set_pixel(strip, i, out_color.r, out_color.g, out_color.b);
        }
        
        led_strip_refresh(strip);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    return ESP_OK;
}

// 设置全局亮度（0-100，百分比）
esp_err_t led_set_brightness(uint8_t brightness_percent)
{
    uint8_t p = brightness_percent;
    if (p > 100) p = 100;
    s_brightness_percent = p;
    s_global_brightness = (float)p / 100.0f;
    return ESP_OK;
}

uint8_t led_get_brightness(void)
{
    return s_brightness_percent;
}

// 设置模式
esp_err_t led_set_mode(led_mode_t mode) {
    current_mode = mode;
    for (int i = 0; i < 256; i++) {
        s_post_prev[i].r = 0;
        s_post_prev[i].g = 0;
        s_post_prev[i].b = 0;
    }
    s_beat.pulse = 0.0f;
    s_beat.onset = false;
    ESP_LOGI(TAG, "LED模式设置为: %d", mode);
    return ESP_OK;
}

// 弹跳小球（原水波纹槽位）：3 颗球受重力弹跳，鼓点齐跳+错落，落地压扁
#define BB_COUNT 3
static struct { float y; float vy; uint32_t bounce; float hue; bool init; } s_ball[BB_COUNT];

static esp_err_t water_ripple_effect(float *energy_bands, int num_bands) {
    (void)energy_bands;
    (void)num_bands;
    if (!strip) return ESP_ERR_INVALID_STATE;

    clear_color_buffer();

    for (int k = 0; k < BB_COUNT; k++) {
        if (!s_ball[k].init) { s_ball[k].y = 0.4f + 0.2f * k; s_ball[k].vy = 0.0f; s_ball[k].bounce = 0; s_ball[k].hue = (float)k / (float)BB_COUNT; s_ball[k].init = true; }
    }

    const led_beat_t *b = led_get_beat();
    float sp = fmaxf(g_fx.speed, 0.2f);
    float a_g = 0.0016f * sp * sp;
    float mag = fminf((b->bass + b->mid + b->high) / 300.0f, 1.0f);
    float kick = 0.030f + fx_beat() * 0.055f + mag * 0.03f;

    for (int k = 0; k < BB_COUNT; k++) {
        if (b->onset) {
            float ph = 0.75f + 0.25f * (float)(((animation_counter / 3) + k) % 3) / 2.0f;
            s_ball[k].vy += kick * ph;
            s_ball[k].hue += 0.04f + 0.10f * fx_beat();
            if (s_ball[k].hue > 1.0f) s_ball[k].hue -= 1.0f;
        }
        s_ball[k].vy -= a_g;
        s_ball[k].y += s_ball[k].vy;
        s_ball[k].hue += 0.0016f * g_fx.color_speed;
        if (s_ball[k].hue > 1.0f) s_ball[k].hue -= 1.0f;
        if (s_ball[k].y <= 0.0f) {
            if (s_ball[k].vy < 0.0f) {
                s_ball[k].bounce = animation_counter;
                s_ball[k].vy = -s_ball[k].vy * 0.55f;
                if (s_ball[k].vy < 0.004f) s_ball[k].vy = 0.004f;
            }
            s_ball[k].y = 0.0f;
        }
        if (s_ball[k].y > 1.0f) { s_ball[k].y = 1.0f; if (s_ball[k].vy > 0.0f) s_ball[k].vy = 0.0f; }
    }

    float lane = (float)led_count / (float)BB_COUNT;
    for (int k = 0; k < BB_COUNT; k++) {
        int lo = (int)floorf(k * lane);
        int hi = (int)ceilf((k + 1) * lane) - 1;
        if (hi >= led_count) hi = led_count - 1;
        if (hi <= lo) { hi = lo + 1 > led_count - 1 ? lo : lo + 1; }

        int age = (int)(animation_counter - s_ball[k].bounce);
        if (age < 0) age = 0;
        float squash = (age < 8) ? (1.0f - (float)age / 8.0f) : 0.0f;
        float sigma = 1.1f + squash * 1.6f;
        float amp = 0.85f + 0.15f * squash;
        float span = (float)(hi - lo);
        float cy = (float)lo + s_ball[k].y * span;
        float hue = s_ball[k].hue;

        for (int led = lo; led <= hi; led++) {
            float dist = (float)led - cy;
            float g = expf(-(dist * dist) / (2.0f * sigma * sigma));
            float bright = g * amp;
            if (bright > 0.02f) {
                float led_hue = hue + dist * 0.02f;
                rgb_color_t color = hsv_to_rgb(led_hue, 0.9f, bright > 1.0f ? 1.0f : bright);
                set_buffer_pixel_blend(led, color, bright);
            }
        }
    }

    return apply_color_buffer();
}

// 增强版能量波效果 - 真正的动态频谱均衡器
static esp_err_t energy_wave_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    // 1. 清空颜色缓冲区
    clear_color_buffer();
    
    // 2. 将频谱分成条带（根据LED数量动态调整）
    int num_bars = 16;
    if (led_count < 32) num_bars = 8; // LED数量少时减少条带数
    if (led_count > 100) num_bars = 24; // LED数量多时增加条带数
    
    float bar_energies[24] = {0}; // 最多24个条带
    int bands_per_bar = num_bands / num_bars;
    if (bands_per_bar < 1) bands_per_bar = 1;
    
    for (int i = 0; i < num_bands; i++) {
        int bar_idx = i / bands_per_bar;
        if (bar_idx >= num_bars) bar_idx = num_bars - 1;
        bar_energies[bar_idx] += energy_bands[i];
    }
    
    // 3. 检测节拍并增强动态效果
    float total_energy = 0;
    float bass_energy = bar_energies[0] + bar_energies[1]; // 低频能量
    
    for (int i = 0; i < num_bars; i++) {
        total_energy += bar_energies[i];
    }
    
    // 检测低频节拍
    static float last_bass_energy = 0;
    uint32_t current_time = animation_counter;
    
    if (bass_energy > 25 && bass_energy > last_bass_energy * 1.5f) {
        // 节拍检测到！
        energy_wave_state.last_beat_time = current_time;
        energy_wave_state.pulse_wave = 1.0f; // 触发脉冲波
        
        // 节拍时色调偏移
        energy_wave_state.hue_shift += 0.1f * g_fx.color_speed;
        if (energy_wave_state.hue_shift > 1.0f) energy_wave_state.hue_shift -= 1.0f;
    }
    last_bass_energy = bass_energy;
    
    // 4. 更新动态效果参数
    energy_wave_state.wave_offset += 0.1f; // 持续移动的波形
    
    // 脉冲波衰减
    if (energy_wave_state.pulse_wave > 0) {
        energy_wave_state.pulse_wave *= 0.9f;
        if (energy_wave_state.pulse_wave < 0.01f) energy_wave_state.pulse_wave = 0;
    }
    
    // 5. 平滑处理条带能量并添加动态效果
    for (int i = 0; i < num_bars; i++) {
        float target = bar_energies[i] / 40.0f; // 归一化到0-1范围
        if (target > 1.5f) target = 1.5f; // 允许稍微超过1
        
        // 指数平滑：快速上升，慢速下降
        if (target > energy_wave_state.smoothed_energies[i]) {
            energy_wave_state.smoothed_energies[i] = energy_wave_state.smoothed_energies[i] * 0.7f + target * 0.3f; // 快速上升
        } else {
            energy_wave_state.smoothed_energies[i] = energy_wave_state.smoothed_energies[i] * 0.95f + target * 0.05f; // 慢速下降
        }
        
        // 添加波动效果
        float wave = sinf(i * 0.3f + energy_wave_state.wave_offset) * 0.1f;
        wave += sinf(i * 0.7f + energy_wave_state.wave_offset * 1.5f) * 0.05f;
        
        // 添加脉冲波效果（节拍时）
        float pulse_effect = sinf(i * 0.5f) * energy_wave_state.pulse_wave * 0.2f;
        
        energy_wave_state.smoothed_energies[i] += wave + pulse_effect;
        if (energy_wave_state.smoothed_energies[i] < 0) energy_wave_state.smoothed_energies[i] = 0;
    }
    
    // 6. 绘制频谱条
    int leds_per_bar = led_count / num_bars;
    if (leds_per_bar < 1) leds_per_bar = 1;
    
    for (int bar = 0; bar < num_bars; bar++) {
        float intensity = energy_wave_state.smoothed_energies[bar];
        
        if (intensity > 0.01f) {
            // 计算条带的起始和结束LED位置
            int start_led = bar * leds_per_bar;
            int end_led = start_led + leds_per_bar - 1;
            if (bar == num_bars - 1) end_led = led_count - 1; // 最后一个条带占用剩余LED
            
            // 计算条带高度（占用的LED数量），添加动态波动
            int bar_height = (int)(intensity * leds_per_bar * 0.8f); // 最大80%高度
            if (bar_height < 1) bar_height = 1;
            if (bar_height > leds_per_bar) bar_height = leds_per_bar;
            
            // 根据频率位置选择颜色，添加动态色调偏移
            rgb_color_t bar_color;
            float bar_pos = (float)bar / num_bars;
            float hue_offset = energy_wave_state.hue_shift;
            
            if (bar_pos < 0.33f) { // 低频：红色到橙色
                float blend = bar_pos / 0.33f;
                float hue = blend * 0.1f + hue_offset; // 红色到橙色渐变
                hue = fmodf(hue, 1.0f);
                bar_color = hsv_to_rgb(hue, 0.9f, 1.0f);
            } else if (bar_pos < 0.66f) { // 中频：绿色到青色
                float blend = (bar_pos - 0.33f) / 0.33f;
                float hue = 0.3f + blend * 0.3f + hue_offset; // 绿色到青色渐变
                hue = fmodf(hue, 1.0f);
                bar_color = hsv_to_rgb(hue, 0.9f, 1.0f);
            } else { // 高频：蓝色到紫色
                float blend = (bar_pos - 0.66f) / 0.34f;
                float hue = 0.6f + blend * 0.2f + hue_offset; // 蓝色到紫色渐变
                hue = fmodf(hue, 1.0f);
                bar_color = hsv_to_rgb(hue, 0.9f, 1.0f);
            }
            
            // 绘制条带（从中心向两边绘制，或者从底部向上绘制）
            int center_led = (start_led + end_led) / 2;
            int half_height = bar_height / 2;
            
            // 添加垂直波动效果
            float vertical_wave = sinf(bar * 0.5f + energy_wave_state.wave_offset * 2.0f) * 0.5f;
            center_led += (int)(vertical_wave * 2.0f);
            
            for (int i = 0; i < bar_height; i++) {
                // 两种绘制方式：1) 从中心向两边 2) 从底部向上
                // 这里采用从中心向两边的方式，看起来更像均衡器
                int led_offset = i - half_height;
                int led_pos1 = center_led + led_offset;
                int led_pos2 = center_led - led_offset;
                
                // 计算这个LED位置的亮度（中心最亮，两端渐暗）
                float position_factor;
                if (bar_height == 1) {
                    position_factor = 1.0f;
                } else {
                    position_factor = 1.0f - fabsf((float)i / bar_height - 0.5f) * 1.5f;
                    if (position_factor < 0.3f) position_factor = 0.3f;
                }
                
                // 整体亮度，添加水平波动
                float wave_factor = 1.0f + sinf(led_offset * 0.3f + energy_wave_state.wave_offset) * 0.1f;
                float led_intensity = intensity * position_factor * wave_factor;
                
                rgb_color_t led_color = {
                    .r = (uint8_t)(bar_color.r * led_intensity),
                    .g = (uint8_t)(bar_color.g * led_intensity),
                    .b = (uint8_t)(bar_color.b * led_intensity)
                };
                
                // 绘制两个对称的LED（如果不同）
                if (led_pos1 >= start_led && led_pos1 <= end_led && led_pos1 >= 0 && led_pos1 < led_count) {
                    set_buffer_pixel(led_pos1, led_color);
                }
                if (led_pos2 >= start_led && led_pos2 <= end_led && led_pos2 != led_pos1 && led_pos2 >= 0 && led_pos2 < led_count) {
                    set_buffer_pixel(led_pos2, led_color);
                }
            }
            
            // 添加条带顶部的亮点（随节拍闪烁）
            if (intensity > 0.5f) {
                float blink = (current_time - energy_wave_state.last_beat_time < 10) ? 1.0f : 0.7f;
                
                rgb_color_t top_color = {
                    .r = (uint8_t)(bar_color.r * 1.2f * blink > 255 ? 255 : bar_color.r * 1.2f * blink),
                    .g = (uint8_t)(bar_color.g * 1.2f * blink > 255 ? 255 : bar_color.g * 1.2f * blink),
                    .b = (uint8_t)(bar_color.b * 1.2f * blink > 255 ? 255 : bar_color.b * 1.2f * blink)
                };
                
                int top_led1 = center_led + half_height;
                int top_led2 = center_led - half_height;
                
                if (top_led1 >= start_led && top_led1 <= end_led && top_led1 >= 0 && top_led1 < led_count) {
                    set_buffer_pixel(top_led1, top_color);
                }
                if (top_led2 >= start_led && top_led2 <= end_led && top_led2 >= 0 && top_led2 < led_count) {
                    set_buffer_pixel(top_led2, top_color);
                }
            }
        }
    }
    
    // 7. 添加峰值保持效果（显示每个条带的峰值）
    for (int i = 0; i < num_bars; i++) {
        if (energy_wave_state.smoothed_energies[i] > energy_wave_state.peak_energies[i]) {
            energy_wave_state.peak_energies[i] = energy_wave_state.smoothed_energies[i];
            energy_wave_state.peak_timers[i] = animation_counter;
        } else {
            // 峰值保持1秒后开始下降
            if (animation_counter - energy_wave_state.peak_timers[i] > 30) { // 假设30帧/秒
                energy_wave_state.peak_energies[i] *= 0.98f; // 缓慢下降
            }
        }
        
        // 绘制峰值点（随节拍闪烁）
        if (energy_wave_state.peak_energies[i] > energy_wave_state.smoothed_energies[i] + 0.1f) {
            // 重新计算leds_per_bar，因为可能在绘制条带之前已经修改过
            int current_leds_per_bar = led_count / num_bars;
            if (current_leds_per_bar < 1) current_leds_per_bar = 1;
            
            int start_led = i * current_leds_per_bar;
            int end_led = start_led + current_leds_per_bar - 1;
            if (i == num_bars - 1) end_led = led_count - 1;
            
            int center_led = (start_led + end_led) / 2;
            int peak_height = (int)(energy_wave_state.peak_energies[i] * current_leds_per_bar * 0.8f);
            int half_height = peak_height / 2;
            
            int peak_led1 = center_led + half_height;
            int peak_led2 = center_led - half_height;
            
            // 白色峰值点，随节拍闪烁
            float blink = (current_time - energy_wave_state.last_beat_time < 5) ? 1.0f : 0.8f;
            rgb_color_t peak_color = {
                .r = (uint8_t)(200 * blink),
                .g = (uint8_t)(200 * blink),
                .b = (uint8_t)(255 * blink)
            };
            
            if (peak_led1 >= start_led && peak_led1 <= end_led && peak_led1 >= 0 && peak_led1 < led_count) {
                set_buffer_pixel(peak_led1, peak_color);
            }
            if (peak_led2 >= start_led && peak_led2 <= end_led && peak_led2 >= 0 && peak_led2 < led_count) {
                set_buffer_pixel(peak_led2, peak_color);
            }
        }
    }
    
    // 8. 添加背景网格线（动态变化）
    if (leds_per_bar > 0) {
        float grid_intensity = 0.3f + sinf(energy_wave_state.wave_offset * 0.5f) * 0.1f;
        for (int i = 0; i < led_count; i += leds_per_bar) {
            if (i < led_count) {
                rgb_color_t grid_color = {
                    .r = (uint8_t)(20 * grid_intensity),
                    .g = (uint8_t)(20 * grid_intensity),
                    .b = (uint8_t)(30 * grid_intensity)
                };
                set_buffer_pixel_blend(i, grid_color, 0.3f);
            }
        }
    }
    
    // 9. 添加节拍粒子效果
    if (current_time - energy_wave_state.last_beat_time < 15) {
        // 节拍后显示粒子效果
        int particle_count = (int)fminf(bass_energy / 20.0f, 10.0f);
        for (int p = 0; p < particle_count; p++) {
            int particle_pos = (int)(((float)rand() / RAND_MAX) * led_count);
            float particle_age = (float)(current_time - energy_wave_state.last_beat_time) / 15.0f;
            float particle_intensity = (1.0f - particle_age) * 0.5f;
            
            if (particle_intensity > 0.01f && particle_pos >= 0 && particle_pos < led_count) {
                rgb_color_t particle_color = hsv_to_rgb(((float)rand() / RAND_MAX), 0.8f, particle_intensity);
                set_buffer_pixel_blend(particle_pos, particle_color, 0.7f);
            }
        }
    }
    
    // 10. 应用颜色缓冲区到LED
    return apply_color_buffer();
}

// 改进的烟花效果
static esp_err_t fireworks_effect_improved(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    // 1. 清空颜色缓冲区
    clear_color_buffer();
    
    // 2. 计算音频能量
    float total_energy = 0;
    float bass_energy = 0;      // 低频能量
    float mid_energy = 0;       // 中频能量
    float treble_energy = 0;    // 高频能量
    
    for (int i = 0; i < num_bands; i++) {
        float energy = energy_bands[i];
        total_energy += energy;
        
        if (i < 2) {  // 低频
            bass_energy += energy;
        } else if (i < 5) {  // 中低频
            mid_energy += energy;
        } else {  // 中高频和高频
            treble_energy += energy;
        }
    }
    
    // 3. 更新能量历史
    fireworks_state.energy_history[fireworks_state.history_index] = total_energy;
    fireworks_state.history_index = (fireworks_state.history_index + 1) % 5;
    
    // 4. 检测能量峰值并触发烟花
    uint32_t current_time = animation_counter;
    
    // 低频能量触发主烟花
    if (bass_energy > 30) {
        // 检查能量是否在上升
        float avg_energy = 0;
        for (int i = 0; i < 5; i++) {
            avg_energy += fireworks_state.energy_history[i];
        }
        avg_energy /= 5.0f;
        
        // 如果当前能量高于平均值，且距离上次烟花有一定时间
        if (total_energy > avg_energy * 1.2f && 
            (current_time - fireworks_state.last_firework_time) > 30) {
            
            // 随机选择爆炸位置
            float position = ((float)rand() / RAND_MAX) * led_count;
            
            // 计算烟花强度
            float intensity = fminf(bass_energy / 80.0f, 1.0f);
            
            // 随机选择颜色
            rgb_color_t color = get_firework_color(rand() % 6);
            
            // 触发烟花爆炸
            trigger_firework_explosion(position, intensity, color);
            fireworks_state.last_firework_time = current_time;
        }
    }
    
    // 中高频能量触发小火花
    if (treble_energy > 25 && (rand() % 15 == 0)) {
        float position = ((float)rand() / RAND_MAX) * led_count;
        float intensity = fminf(treble_energy / 60.0f, 0.6f);
        rgb_color_t color = get_firework_color(rand() % 6);
        
        trigger_firework_explosion(position, intensity, color);
    }
    
    // 5. 更新爆炸点
    for (int i = 0; i < 5; i++) {
        if (fireworks_state.explosions[i].active) {
            // 计算爆炸已持续的时间
            uint32_t age = current_time - fireworks_state.explosions[i].start_time;
            
            if (age < 30) {  // 爆炸持续阶段
                // 计算当前爆炸强度（随时间衰减）
                float intensity = fireworks_state.explosions[i].intensity;
                float age_factor = 1.0f - (float)age / 30.0f; // 0到1之间
                
                // 计算爆炸半径（随时间扩大）
                float base_radius = fireworks_state.explosions[i].radius;
                float current_radius = base_radius * (1.0f + age * 0.2f);
                
                rgb_color_t color = fireworks_state.explosions[i].color;
                
                // 绘制爆炸效果
                for (int led = 0; led < led_count; led++) {
                    float distance = fabsf((float)led - fireworks_state.explosions[i].position);
                    
                    if (distance <= current_radius) {
                        // 计算爆炸强度衰减
                        float radial_factor = 1.0f - (distance / current_radius);
                        float brightness = intensity * age_factor * radial_factor * radial_factor;
                        
                        if (brightness > 0.01f) {
                            rgb_color_t explosion_color = {
                                .r = (uint8_t)(color.r * brightness),
                                .g = (uint8_t)(color.g * brightness),
                                .b = (uint8_t)(color.b * brightness)
                            };
                            
                            // 混合到颜色缓冲区
                            set_buffer_pixel_blend(led, explosion_color, 1.0f);
                        }
                    }
                }
            } else if (age > 40) {  // 爆炸结束
                fireworks_state.explosions[i].active = false;
            }
        }
    }
    
    // 6. 更新粒子
    for (int i = 0; i < 30; i++) {
        if (fireworks_state.particles[i].active) {
            // 更新粒子位置和亮度
            fireworks_state.particles[i].position += fireworks_state.particles[i].velocity * g_fx.speed;
            fireworks_state.particles[i].lifetime--;
            
            // 更新粒子亮度（指数衰减）
            fireworks_state.particles[i].brightness *= 0.94f;
            
            // 绘制粒子
            int pos = (int)fireworks_state.particles[i].position;
            float brightness = fireworks_state.particles[i].brightness;
            
            if (pos >= 0 && pos < led_count && brightness > 0.05f) {
                rgb_color_t color = fireworks_state.particles[i].color;
                
                // 粒子中心
                rgb_color_t center_color = {
                    .r = (uint8_t)(color.r * brightness),
                    .g = (uint8_t)(color.g * brightness),
                    .b = (uint8_t)(color.b * brightness)
                };
                set_buffer_pixel_blend(pos, center_color, 1.0f);
                
                // 粒子拖尾效果
                for (int offset = 1; offset <= 3; offset++) {
                    int trail_pos = pos - (int)(fireworks_state.particles[i].velocity > 0 ? offset : -offset);
                    if (trail_pos >= 0 && trail_pos < led_count) {
                        float trail_brightness = brightness * (0.4f / offset);
                        rgb_color_t trail_color = {
                            .r = (uint8_t)(color.r * trail_brightness),
                            .g = (uint8_t)(color.g * trail_brightness),
                            .b = (uint8_t)(color.b * trail_brightness)
                        };
                        set_buffer_pixel_blend(trail_pos, trail_color, 1.0f);
                    }
                }
            }
            
            // 检查粒子是否结束
            if (fireworks_state.particles[i].lifetime <= 0 || 
                fireworks_state.particles[i].brightness < 0.05f ||
                fireworks_state.particles[i].position < -10 || 
                fireworks_state.particles[i].position > led_count + 10) {
                fireworks_state.particles[i].active = false;
            }
        }
    }
    
    // 7. 添加音频响应背景效果
    float background_level = total_energy / 300.0f;
    if (background_level > 0.02f) {
        for (int i = 0; i < led_count; i++) {
            // 根据频率分布添加渐变背景
            int band_idx = (i * num_bands) / led_count;
            if (band_idx >= num_bands) band_idx = num_bands - 1;
            
            float band_energy = energy_bands[band_idx] / 80.0f;
            
            if (band_energy > 0.01f) {
                // 背景颜色根据频率变化
                rgb_color_t bg_color;
                if (band_idx < num_bands / 4) {
                    // 低频：深蓝色
                    bg_color = (rgb_color_t){
                        0, 
                        (uint8_t)(band_energy * 20 * background_level), 
                        (uint8_t)(band_energy * 40 * background_level)
                    };
                } else if (band_idx < num_bands / 2) {
                    // 中低频：深紫色
                    bg_color = (rgb_color_t){
                        (uint8_t)(band_energy * 15 * background_level), 
                        0, 
                        (uint8_t)(band_energy * 30 * background_level)
                    };
                } else {
                    // 中高频：深红色
                    bg_color = (rgb_color_t){
                        (uint8_t)(band_energy * 25 * background_level), 
                        0, 
                        0
                    };
                }
                
                // 混合到颜色缓冲区（低混合因子，避免覆盖烟花）
                set_buffer_pixel_blend(i, bg_color, 0.3f);
            }
        }
    }
    
    // 8. 应用颜色缓冲区到LED
    return apply_color_buffer();
}

// 改进的节奏脉冲效果 - 随机发射脉冲
static esp_err_t rhythm_pulse_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    // 1. 清空颜色缓冲区
    clear_color_buffer();
    
    // 2. 计算音频能量（按频率分组）
    float band_energies[6] = {0}; // 分成6个频段
    
    for (int i = 0; i < num_bands; i++) {
        int band = (i * 6) / num_bands;
        if (band >= 6) band = 5;
        band_energies[band] += energy_bands[i];
    }
    
    // 提取重要频段
    float bass_energy = band_energies[0] + band_energies[1];     // 超低频和低频
    float mid_energy = band_energies[2] + band_energies[3];      // 中低频和中频
    float treble_energy = band_energies[4] + band_energies[5];   // 高频和超高频
    
    // 3. 检测节奏并发射脉冲
    uint32_t current_time = animation_counter;
    
    // 低频节奏检测（鼓点）
    static float last_bass_energy = 0;
    static uint32_t last_bass_pulse = 0;
    
    if (bass_energy > 25 && bass_energy > last_bass_energy * 1.3f) {
        // 触发脉冲
        if (current_time - last_bass_pulse > 15) {
            // 随机选择发射方向：0=从左向右，1=从右向左
            int direction = rand() % 2;
            
            // 随机选择颜色（基于频率）
            rgb_color_t pulse_color;
            int color_type = rand() % 6;
            
            switch (color_type) {
                case 0: pulse_color = (rgb_color_t){255, 50, 50}; break;    // 红色
                case 1: pulse_color = (rgb_color_t){255, 150, 30}; break;   // 橙色
                case 2: pulse_color = (rgb_color_t){255, 255, 50}; break;   // 黄色
                case 3: pulse_color = (rgb_color_t){50, 255, 50}; break;    // 绿色
                case 4: pulse_color = (rgb_color_t){50, 150, 255}; break;   // 蓝色
                case 5: pulse_color = (rgb_color_t){200, 50, 255}; break;   // 紫色
            }
            
            // 随机选择长度（3-8个LED）
            int pulse_length = 3 + (rand() % 6);
            
            // 随机选择速度
            float pulse_speed = 0.5f + (rand() % 100) / 100.0f * 1.5f;
            
            // 创建脉冲
            create_pulse(direction, pulse_color, pulse_length, pulse_speed, bass_energy / 80.0f);
            
            last_bass_pulse = current_time;
        }
    }
    last_bass_energy = bass_energy;
    
    // 中频节奏检测（人声/乐器）
    static float last_mid_energy = 0;
    static uint32_t last_mid_pulse = 0;
    
    if (mid_energy > 30 && mid_energy > last_mid_energy * 1.5f) {
        if (current_time - last_mid_pulse > 20) {
            int direction = rand() % 2;
            
            // 中频使用更柔和的颜色
            rgb_color_t pulse_color;
            int color_type = rand() % 4;
            
            switch (color_type) {
                case 0: pulse_color = (rgb_color_t){255, 100, 100}; break;  // 粉红
                case 1: pulse_color = (rgb_color_t){100, 255, 100}; break;  // 浅绿
                case 2: pulse_color = (rgb_color_t){100, 100, 255}; break;  // 浅蓝
                case 3: pulse_color = (rgb_color_t){255, 255, 100}; break;  // 浅黄
            }
            
            int pulse_length = 2 + (rand() % 4);
            float pulse_speed = 0.8f + (rand() % 100) / 100.0f * 1.2f;
            
            create_pulse(direction, pulse_color, pulse_length, pulse_speed, mid_energy / 100.0f);
            
            last_mid_pulse = current_time;
        }
    }
    last_mid_energy = mid_energy;
    
    // 高频节奏检测（镲片/高音乐器）
    static float last_treble_energy = 0;
    static uint32_t last_treble_pulse = 0;
    
    if (treble_energy > 35 && treble_energy > last_treble_energy * 2.0f) {
        if (current_time - last_treble_pulse > 8) {
            int direction = rand() % 2;
            
            // 高频使用明亮颜色
            rgb_color_t pulse_color;
            int color_type = rand() % 3;
            
            switch (color_type) {
                case 0: pulse_color = (rgb_color_t){255, 255, 200}; break;  // 亮白
                case 1: pulse_color = (rgb_color_t){200, 255, 255}; break;  // 亮青
                case 2: pulse_color = (rgb_color_t){255, 200, 255}; break;  // 亮紫
            }
            
            int pulse_length = 1 + (rand() % 3);
            float pulse_speed = 1.5f + (rand() % 100) / 100.0f * 2.0f;
            
            create_pulse(direction, pulse_color, pulse_length, pulse_speed, treble_energy / 120.0f);
            
            last_treble_pulse = current_time;
        }
    }
    last_treble_energy = treble_energy;
    
    // 4. 更新和绘制所有脉冲
    update_and_draw_pulses();
    
    // 5. 添加频率能量背景显示
    for (int i = 0; i < led_count; i++) {
        // 将LED位置映射到频段
        int band_idx = (i * 6) / led_count;
        if (band_idx >= 6) band_idx = 5;
        
        float band_energy = band_energies[band_idx] / 50.0f;
        
        if (band_energy > 0.01f) {
            // 背景能量显示（暗色调）
            rgb_color_t bg_color;
            
            // 不同频段不同颜色
            switch (band_idx) {
                case 0: // 超低频：深红
                    bg_color = (rgb_color_t){ (uint8_t)(band_energy * 40), 0, 0 };
                    break;
                case 1: // 低频：深橙
                    bg_color = (rgb_color_t){ (uint8_t)(band_energy * 50), (uint8_t)(band_energy * 20), 0 };
                    break;
                case 2: // 中低频：深黄
                    bg_color = (rgb_color_t){ (uint8_t)(band_energy * 60), (uint8_t)(band_energy * 60), 0 };
                    break;
                case 3: // 中频：深绿
                    bg_color = (rgb_color_t){ 0, (uint8_t)(band_energy * 50), 0 };
                    break;
                case 4: // 高频：深蓝
                    bg_color = (rgb_color_t){ 0, 0, (uint8_t)(band_energy * 60) };
                    break;
                case 5: // 超高频：深紫
                    bg_color = (rgb_color_t){ (uint8_t)(band_energy * 30), 0, (uint8_t)(band_energy * 50) };
                    break;
                default:
                    bg_color = (rgb_color_t){0, 0, 0};
                    break;
            }
            
            // 添加微弱的脉冲效果
            float pulse_mod = sinf(animation_counter * 0.1f + i * 0.2f) * 0.1f + 0.9f;
            bg_color.r = (uint8_t)(bg_color.r * pulse_mod);
            bg_color.g = (uint8_t)(bg_color.g * pulse_mod);
            bg_color.b = (uint8_t)(bg_color.b * pulse_mod);
            
            set_buffer_pixel_blend(i, bg_color, 0.4f);
        }
    }
    
    // 6. 应用颜色缓冲区到LED
    return apply_color_buffer();
}

// 流星脉冲效果实现
// 初始化流星脉冲
static void init_meteor_pulse(int index, float total_energy, float bass_energy, 
                              float mid_energy, float high_energy, float energy_variance) {
    if (index < 0 || index >= MAX_METEORS) return;
    
    meteor_pulse_t *meteor = &meteor_pulse_state.meteors[index];
    
    meteor->active = true;
    meteor->age = 0;
    
    // 随机方向
    meteor->direction = (rand() % 2 == 0) ? 1 : -1;
    
    // 根据能量决定起始位置
    if (meteor->direction > 0) {
        meteor->position = -5.0f - (rand() % 10);
    } else {
        meteor->position = led_count + 5.0f + (rand() % 10);
    }
    
    // 动态速度：能量越高速度越快
    float speed_factor = fminf(total_energy / 80.0f, 1.0f);
    meteor->speed = (meteor_pulse_state.min_speed + 
                    (meteor_pulse_state.max_speed - meteor_pulse_state.min_speed) * speed_factor) * g_fx.speed;
    
    // 动态长度：能量变化越大，流星越长
    float length_factor = fminf(energy_variance * 2.0f, 1.0f);
    meteor->length = meteor_pulse_state.min_length + 
                    (int)((meteor_pulse_state.max_length - meteor_pulse_state.min_length) * length_factor);
    
    // 动态亮度：根据总能量
    meteor->max_brightness = 0.7f + fminf(total_energy / 100.0f, 0.3f);
    meteor->brightness = meteor->max_brightness;
    meteor->fade_speed = 0.02f + (rand() % 10) / 1000.0f;
    
    // 动态尾迹宽度：根据长度
    meteor->trail_width = 0.5f + (meteor->length * 0.05f);
    
    // 颜色生成：根据频谱能量分布
    meteor_pulse_state.color_hue += meteor_pulse_state.hue_speed * g_fx.color_speed;
    if (meteor_pulse_state.color_hue > 1.0f) {
        meteor_pulse_state.color_hue -= 1.0f;
    }
    
    float hue = meteor_pulse_state.color_hue;
    
    // 根据能量类型调整颜色
    if (bass_energy > mid_energy && bass_energy > high_energy) {
        // 低频主导：暖色系（红、橙）
        hue = fmodf(hue + 0.0f, 1.0f);
        meteor->head_color = hsv_to_rgb(hue, 0.9f, 1.0f);
        meteor->tail_color = hsv_to_rgb(fmodf(hue + 0.05f, 1.0f), 0.7f, 0.8f);
    } else if (mid_energy > bass_energy && mid_energy > high_energy) {
        // 中频主导：中性色系（绿、青）
        hue = fmodf(hue + 0.35f, 1.0f);
        meteor->head_color = hsv_to_rgb(hue, 0.85f, 1.0f);
        meteor->tail_color = hsv_to_rgb(fmodf(hue + 0.03f, 1.0f), 0.6f, 0.7f);
    } else {
        // 高频主导：冷色系（蓝、紫）
        hue = fmodf(hue + 0.65f, 1.0f);
        meteor->head_color = hsv_to_rgb(hue, 0.8f, 1.0f);
        meteor->tail_color = hsv_to_rgb(fmodf(hue + 0.02f, 1.0f), 0.5f, 0.6f);
    }
    
    // 动态寿命：速度越快，寿命越短
    meteor->lifespan = (float)(led_count) / meteor->speed * 1.2f;
    
    // 是否带有闪烁效果（根据能量变化率决定）
    meteor->has_sparkle = (energy_variance > 0.3f) && (rand() % 100 < 50);
    meteor->sparkle_intensity = energy_variance * 0.5f;
    
    // 随机添加颜色偏移，使每个流星颜色略有不同
    float color_shift = ((rand() % 20) - 10) / 100.0f;
    rgb_color_t adjusted_head = meteor->head_color;
    rgb_color_t adjusted_tail = meteor->tail_color;
    
    adjusted_head.r = (uint8_t)(adjusted_head.r * (1.0f + color_shift));
    adjusted_head.g = (uint8_t)(adjusted_head.g * (1.0f - color_shift * 0.5f));
    adjusted_tail.b = (uint8_t)(adjusted_tail.b * (1.0f + color_shift));
    
    meteor->head_color = adjusted_head;
    meteor->tail_color = adjusted_tail;
}

// 计算能量方差（用于检测节奏变化）
static float calculate_energy_variance(float *energy_bands, int num_bands, float current_energy) {
    // 更新能量历史
    meteor_pulse_state.energy_history[meteor_pulse_state.energy_history_index] = current_energy;
    meteor_pulse_state.energy_history_index = (meteor_pulse_state.energy_history_index + 1) % 10;
    
    // 计算方差
    float sum = 0;
    float count = 0;
    
    for (int i = 0; i < 10; i++) {
        if (meteor_pulse_state.energy_history[i] > 0) {
            sum += meteor_pulse_state.energy_history[i];
            count++;
        }
    }
    
    if (count < 2) return 0;
    
    float mean = sum / count;
    float variance = 0;
    
    for (int i = 0; i < 10; i++) {
        if (meteor_pulse_state.energy_history[i] > 0) {
            float diff = meteor_pulse_state.energy_history[i] - mean;
            variance += diff * diff;
        }
    }
    
    variance /= (count - 1);
    
    // 归一化方差（假设最大方差为100）
    return fminf(sqrtf(variance) / 10.0f, 1.0f);
}

// 获取空闲的流星槽位
static int get_free_meteor_slot(void) {
    for (int i = 0; i < MAX_METEORS; i++) {
        if (!meteor_pulse_state.meteors[i].active) {
            return i;
        }
    }
    return -1;
}

// 更新所有流星
static void update_meteors(void) {
    for (int i = 0; i < MAX_METEORS; i++) {
        if (!meteor_pulse_state.meteors[i].active) continue;
        
        meteor_pulse_t *meteor = &meteor_pulse_state.meteors[i];
        
        // 更新位置
        meteor->position += meteor->speed * meteor->direction;
        meteor->age++;
        
        // 更新亮度（逐渐衰减）
        meteor->brightness -= meteor->fade_speed;
        if (meteor->brightness < 0.1f) meteor->brightness = 0.1f;
        
        // 检查是否超出边界或寿命结束
        bool out_of_bounds = false;
        if (meteor->direction > 0) {
            out_of_bounds = meteor->position > led_count + meteor->length;
        } else {
            out_of_bounds = meteor->position < -meteor->length;
        }
        
        if (out_of_bounds || meteor->age > meteor->lifespan) {
            meteor->active = false;
        }
    }
}

// 绘制单个流星
static void draw_meteor(meteor_pulse_t *meteor) {
    if (!meteor || !meteor->active) return;
    
    int head_pos = (int)meteor->position;
    int direction = meteor->direction;
    
    // 绘制流星主体（从头部到尾部）
    for (int i = 0; i < meteor->length; i++) {
        int led_pos;
        if (direction > 0) {
            led_pos = head_pos - i;  // 向右移动，头部在最右边
        } else {
            led_pos = head_pos + i;  // 向左移动，头部在最左边
        }
        
        if (led_pos < 0 || led_pos >= led_count) continue;
        
        // 计算该LED在流星中的位置比例（0=头部，1=尾部）
        float position_factor = (float)i / meteor->length;
        
        // 计算颜色插值：从头部颜色渐变到尾部颜色
        rgb_color_t color;
        color.r = (uint8_t)(meteor->head_color.r * (1.0f - position_factor) + 
                           meteor->tail_color.r * position_factor);
        color.g = (uint8_t)(meteor->head_color.g * (1.0f - position_factor) + 
                           meteor->tail_color.g * position_factor);
        color.b = (uint8_t)(meteor->head_color.b * (1.0f - position_factor) + 
                           meteor->tail_color.b * position_factor);
        
        // 计算亮度衰减：头部最亮，尾部逐渐变暗
        float brightness_factor;
        if (i == 0) {
            // 头部：最亮
            brightness_factor = meteor->brightness;
        } else {
            // 身体：指数衰减
            brightness_factor = meteor->brightness * expf(-position_factor * 3.0f);
        }
        
        // 应用亮度
        color.r = (uint8_t)(color.r * brightness_factor);
        color.g = (uint8_t)(color.g * brightness_factor);
        color.b = (uint8_t)(color.b * brightness_factor);
        
        // 设置LED颜色
        set_buffer_pixel(led_pos, color);
        
        // 添加光晕效果（尾迹）
        if (meteor->trail_width > 0.5f && i > 0) {
            float glow_intensity = brightness_factor * 0.3f;
            rgb_color_t glow_color = {
                .r = (uint8_t)(color.r * glow_intensity),
                .g = (uint8_t)(color.g * glow_intensity),
                .b = (uint8_t)(color.b * glow_intensity)
            };
            
            // 向两侧扩散光晕
            for (int offset = 1; offset <= (int)meteor->trail_width; offset++) {
                if (led_pos - offset >= 0) {
                    float distance_factor = 1.0f - (float)offset / meteor->trail_width;
                    set_buffer_pixel_blend(led_pos - offset, glow_color, 0.3f * distance_factor);
                }
                if (led_pos + offset < led_count) {
                    float distance_factor = 1.0f - (float)offset / meteor->trail_width;
                    set_buffer_pixel_blend(led_pos + offset, glow_color, 0.3f * distance_factor);
                }
            }
        }
    }   
    
    // 头部特效
    if (head_pos >= 0 && head_pos < led_count) {
        // 头部高亮
        float head_brightness = meteor->brightness * 1.2f;
        if (head_brightness > 1.0f) head_brightness = 1.0f;
        
        rgb_color_t head_color = {
            .r = (uint8_t)(meteor->head_color.r * head_brightness),
            .g = (uint8_t)(meteor->head_color.g * head_brightness),
            .b = (uint8_t)(meteor->head_color.b * head_brightness)
        };
        
        set_buffer_pixel_blend(head_pos, head_color, 1.0f);
        
        // 头部光晕
        for (int offset = 1; offset <= 3; offset++) {
            float glow_intensity = head_brightness * (0.5f / offset);
            rgb_color_t glow_color = {
                .r = (uint8_t)(head_color.r * glow_intensity),
                .g = (uint8_t)(head_color.g * glow_intensity),
                .b = (uint8_t)(head_color.b * glow_intensity)
            };
            
            if (head_pos - offset >= 0) {
                set_buffer_pixel_blend(head_pos - offset, glow_color, 0.6f);
            }
            if (head_pos + offset < led_count) {
                set_buffer_pixel_blend(head_pos + offset, glow_color, 0.6f);
            }
        }
        
        // 闪烁效果
        if (meteor->has_sparkle && (meteor_pulse_state.frame_counter % 5) == 0) {
            int sparkle_pos = head_pos + (rand() % 5) - 2;
            if (sparkle_pos >= 0 && sparkle_pos < led_count) {
                float sparkle_brightness = meteor->sparkle_intensity * (0.7f + (rand() % 30) / 100.0f);
                rgb_color_t sparkle_color = {
                    .r = (uint8_t)(255 * sparkle_brightness),
                    .g = (uint8_t)(220 * sparkle_brightness),
                    .b = (uint8_t)(180 * sparkle_brightness)
                };
                set_buffer_pixel_blend(sparkle_pos, sparkle_color, 0.8f);
            }
        }
    }
    
    // 绘制流星轨迹上的粒子
    if (meteor->age < meteor->lifespan * 0.8f && meteor->speed > 1.5f) {
        for (int p = 0; p < 2; p++) {
            int particle_offset = rand() % meteor->length;
            int particle_pos;
            if (direction > 0) {
                particle_pos = head_pos - particle_offset;
            } else {
                particle_pos = head_pos + particle_offset;
            }
            
            if (particle_pos >= 0 && particle_pos < led_count) {
                float particle_brightness = meteor->brightness * (0.3f + (rand() % 20) / 100.0f);
                rgb_color_t particle_color = {
                    .r = (uint8_t)(meteor->tail_color.r * particle_brightness),
                    .g = (uint8_t)(meteor->tail_color.g * particle_brightness),
                    .b = (uint8_t)(meteor->tail_color.b * particle_brightness)
                };
                set_buffer_pixel_blend(particle_pos, particle_color, 0.5f);
            }
        }
    }
}

// 流星脉冲效果主函数
static esp_err_t meteor_pulse_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    clear_color_buffer();
    
    meteor_pulse_state.frame_counter++;
    
    // 计算音频能量
    float bass_energy = 0, mid_energy = 0, high_energy = 0, total_energy = 0;
    
    if (energy_bands && num_bands > 0) {
        for (int i = 0; i < num_bands; i++) {
            float energy = energy_bands[i];
            total_energy += energy;
            
            if (i < num_bands / 3) {
                bass_energy += energy;
            } else if (i < num_bands * 2 / 3) {
                mid_energy += energy;
            } else {
                high_energy += energy;
            }
        }
        
        // 归一化
        if (num_bands > 0) {
            bass_energy /= num_bands / 3;
            mid_energy /= num_bands / 3;
            high_energy /= num_bands - (num_bands * 2 / 3);
            total_energy /= num_bands;
        }
    }
    
    // 计算能量方差（节奏变化）
    float energy_variance = calculate_energy_variance(energy_bands, num_bands, total_energy);
    
    // 判断是否应该触发新的流星
    uint32_t current_time = animation_counter;
    bool should_trigger = false;
    
    // 1. 基于能量方差（节奏变化）触发
    if (energy_variance > 0.5f) {
        // 高节奏变化时，触发概率与方差成正比
        int trigger_chance = (int)(energy_variance * 60);
        if (rand() % 100 < trigger_chance) {
            should_trigger = true;
        }
    }
    
    // 2. 基于总能量触发
    if (total_energy > 35.0f) {
        // 动态触发间隔：能量越高，间隔越小
        float dynamic_interval = meteor_pulse_state.max_interval - 
                               (meteor_pulse_state.max_interval - meteor_pulse_state.min_interval) * 
                               fminf(total_energy / 120.0f, 1.0f);
        
        if (current_time - meteor_pulse_state.last_trigger_time > dynamic_interval) {
            should_trigger = true;
        }
    }
    
    // 3. 基于能量峰值触发
    static float last_energy = 0;
    if (total_energy > last_energy * 1.8f && total_energy > 30.0f) {
        should_trigger = true;
    }
    last_energy = total_energy;
    
    // 4. 确保至少有一些流星活动
    int active_meteors = 0;
    for (int i = 0; i < MAX_METEORS; i++) {
        if (meteor_pulse_state.meteors[i].active) active_meteors++;
    }
    
    if (active_meteors == 0 && current_time - meteor_pulse_state.last_trigger_time > 100) {
        should_trigger = true;
    }
    
    // 触发新的流星
    if (should_trigger) {
        int free_slot = get_free_meteor_slot();
        if (free_slot >= 0) {
            init_meteor_pulse(free_slot, total_energy, bass_energy, mid_energy, high_energy, energy_variance);
            meteor_pulse_state.last_trigger_time = current_time;
            
            // 如果有高能量变化，可以同时触发多个流星
            if (energy_variance > 0.7f && total_energy > 50.0f) {
                int second_slot = get_free_meteor_slot();
                if (second_slot >= 0 && rand() % 100 < 30) { 
                    // 第二个流星方向相反
                    init_meteor_pulse(second_slot, total_energy, bass_energy, mid_energy, high_energy, energy_variance);
                    // 确保方向相反
                    meteor_pulse_state.meteors[second_slot].direction *= -1;
                }
            }
        }
    }
    
    // 更新所有流星
    update_meteors();
    
    // 绘制所有流星
    for (int i = 0; i < MAX_METEORS; i++) {
        if (meteor_pulse_state.meteors[i].active) {
            draw_meteor(&meteor_pulse_state.meteors[i]);
        }
    }
    
    // 背景效果：根据低频能量显示微弱背景光
    float background_level = bass_energy / 50.0f;
    if (background_level > 0.05f) {
        for (int i = 0; i < led_count; i++) {
            // 创建流动的背景光点
            float wave_pos = (float)i / led_count * 2 * M_PI;
            float time_factor = (float)meteor_pulse_state.frame_counter * 0.05f;
            float wave_value = sinf(wave_pos + time_factor) * 0.5f + 0.5f;
            
            float brightness = background_level * 0.1f * wave_value;
            
            // 颜色跟随基础色调变化
            float bg_hue = fmodf(meteor_pulse_state.color_hue + 0.5f, 1.0f);
            rgb_color_t bg_color = hsv_to_rgb(bg_hue, 0.3f, brightness);
            
            set_buffer_pixel_blend(i, bg_color, 0.3f);
        }
    }
    
    return apply_color_buffer();
}
// 节奏呼吸效果 - 音频包络驱动的径向呼吸（混合式：能量包络+鼓点深呼吸+BPM微起伏）
static esp_err_t rhythm_breath_effect(float *energy_bands, int num_bands) {
    (void)energy_bands;
    (void)num_bands;
    if (!strip) return ESP_ERR_INVALID_STATE;

    clear_color_buffer();

    const led_beat_t *b = led_get_beat();

    float loud = fminf(b->energy / 120.0f, 1.0f);

    static float s_env = 0.0f;
    static float s_hue = 0.0f;
    if (loud > s_env) {
        s_env += (loud - s_env) * 0.5f;
    } else {
        float release = 0.03f / fmaxf(g_fx.speed, 0.2f);
        s_env += (loud - s_env) * release;
    }
    if (b->onset) {
        s_env = fminf(s_env + 0.35f, 1.0f);
    }
    float env = fminf(s_env + fx_beat() * 0.4f, 1.0f);

    float sum = b->bass + b->mid + b->high + 0.001f;
    float tone = (b->mid / sum) * 0.5f + (b->high / sum) * 1.0f;
    s_hue += 0.0009f * g_fx.color_speed;
    if (s_hue > 1.0f) s_hue -= 1.0f;
    float base_hue = tone * 0.62f + s_hue;

    float bpm = b->bpm ? (float)b->bpm : 72.0f;
    rhythm_breath_state.breath_phase += 2.0f * 3.1416f * (bpm / 60.0f) * 0.01f * g_fx.speed;
    if (rhythm_breath_state.breath_phase > 2.0f * 3.1416f) {
        rhythm_breath_state.breath_phase -= 2.0f * 3.1416f;
    }
    float bpm_osc = 0.5f + 0.5f * sinf(rhythm_breath_state.breath_phase);

    float center = (led_count - 1) * 0.5f;
    if (center < 1.0f) center = 1.0f;
    float width = 0.35f + 1.15f * env;

    for (int i = 0; i < led_count; i++) {
        float d = fabsf((float)i - center) / center;
        float radial = fmaxf(0.0f, 1.0f - d / width);
        radial = radial * radial;

        float amp = (0.28f + 0.72f * env) * radial + 0.10f * bpm_osc * (1.0f - d);
        if (amp > 1.0f) amp = 1.0f;

        float value = rhythm_breath_state.breath_min +
                      (rhythm_breath_state.breath_max - rhythm_breath_state.breath_min) * amp;
        float hue = base_hue + 0.12f * d;
        float sat = 0.9f - 0.25f * env;
        if (sat < 0.5f) sat = 0.5f;

        rgb_color_t color = hsv_to_rgb(hue, sat, value);
        set_buffer_pixel(i, color);
    }

    return apply_color_buffer();
}

// 初始化跳跃段
static void init_jump_segments(void) {
    for (int i = 0; i < 8; i++) {
        rhythm_jump_state.segments[i].height = 0.1f;  // 初始高度不为0
        rhythm_jump_state.segments[i].velocity = 0;
        rhythm_jump_state.segments[i].target_height = 0.1f;
        rhythm_jump_state.segments[i].mass = 0.8f;    // 减小质量，更灵敏
        rhythm_jump_state.segments[i].stiffness = 0.3f + (i * 0.04f);  // 增加刚度
        rhythm_jump_state.segments[i].damping = 0.85f; // 减小阻尼，跳动更持久
        
        // 初始化峰值
        rhythm_jump_state.energy_peak[i] = 0;
        
        // 使用更鲜艳的颜色
        float hue = (float)i / 8.0f;
        rhythm_jump_state.segments[i].color = hsv_to_rgb(hue, 0.8f, 1.0f); // 饱和度和亮度都设为1
    }
}

static void update_physics_simulation(void) {
    for (int i = 0; i < 8; i++) {
        float displacement = rhythm_jump_state.segments[i].target_height - 
                           rhythm_jump_state.segments[i].height;
        
        // 增强的弹簧力计算
        float spring_force = rhythm_jump_state.segments[i].stiffness * displacement * 1.2f;
        
        // 非线性阻尼，速度越快阻尼越大
        float damping_force = rhythm_jump_state.segments[i].damping * 
                            rhythm_jump_state.segments[i].velocity * 
                            (1.0f + fabsf(rhythm_jump_state.segments[i].velocity) * 0.5f);
        
        // 动态重力，高度越高重力越大
        float dynamic_gravity = rhythm_jump_state.gravity * 
                              (1.0f + rhythm_jump_state.segments[i].height * 0.5f);
        float gravity_force = -dynamic_gravity * 
                            rhythm_jump_state.segments[i].height;
        
        // 增加随机微扰，使运动更自然
        float random_force = 0;
        if (rand() % 100 < 30) { // 30%概率添加微扰
            random_force = (rand() % 100 - 50) / 500.0f;
        }
        
        float acceleration = (spring_force - damping_force + gravity_force + random_force) / 
                           rhythm_jump_state.segments[i].mass;
        
        rhythm_jump_state.segments[i].velocity += acceleration * 0.12f; // 增加时间步长
        rhythm_jump_state.segments[i].height += rhythm_jump_state.segments[i].velocity;
        
        // 边界处理，增加弹性碰撞效果
        if (rhythm_jump_state.segments[i].height < 0.05f) {
            rhythm_jump_state.segments[i].height = 0.05f;
            rhythm_jump_state.segments[i].velocity = -rhythm_jump_state.segments[i].velocity * 0.7f;
        }
        if (rhythm_jump_state.segments[i].height > 1.5f) { // 允许跳得更高
            rhythm_jump_state.segments[i].height = 1.5f;
            rhythm_jump_state.segments[i].velocity = -rhythm_jump_state.segments[i].velocity * 0.4f;
        }
    }
}

static esp_err_t rhythm_jump_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    clear_color_buffer();
    
    static bool initialized = false;
    if (!initialized) {
        init_jump_segments();
        initialized = true;
    }
    
    // 计算全局能量，用于增强整体响应
    float global_energy = 0;
    if (energy_bands && num_bands > 0) {
        for (int i = 0; i < num_bands; i++) {
            global_energy += energy_bands[i];
        }
        global_energy /= num_bands;
    }

    float rj_pulse = s_beat.pulse * (0.5f + g_fx.beat_react);
    
    float band_energies[8] = {0};
    if (energy_bands && num_bands > 0) {
        int bands_per_segment = num_bands / 8;
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < bands_per_segment && (i * bands_per_segment + j) < num_bands; j++) {
                band_energies[i] += energy_bands[i * bands_per_segment + j];
            }
            band_energies[i] /= bands_per_segment;
        }
    }
    
    for (int i = 0; i < 8; i++) {
        // 使用更快的平滑，响应更迅速
        float smooth_factor = 0.6f;
        rhythm_jump_state.energy_history[i] = 
            rhythm_jump_state.energy_history[i] * smooth_factor + 
            band_energies[i] * (1.0f - smooth_factor);
        
        // 更新能量峰值
        if (band_energies[i] > rhythm_jump_state.energy_peak[i]) {
            rhythm_jump_state.energy_peak[i] = band_energies[i];
        } else {
            rhythm_jump_state.energy_peak[i] *= rhythm_jump_state.peak_decay;
        }
        
        float energy = rhythm_jump_state.energy_history[i];
        float peak_energy = rhythm_jump_state.energy_peak[i];
        
        // 增强的能量映射
        float target_height = 0;
        
        // 低频段（0-1）响应更强
        if (i < 2) {
            target_height = fminf(energy / 25.0f, 1.5f);  // 降低分母，增加高度
            if (peak_energy > 40) {
                target_height *= 1.0f;  // 峰值时额外增强
            }
            rhythm_jump_state.segments[i].stiffness = 0.25f;
        } 
        // 中频段（2-4）
        else if (i < 5) {
            target_height = fminf(energy / 35.0f, 1.2f);
            if (peak_energy > 50) {
                target_height *= 1.2f;
            }
            rhythm_jump_state.segments[i].stiffness = 0.35f;
        } 
        // 高频段（5-7）
        else {
            target_height = fminf(energy / 45.0f, 1.0f);
            if (peak_energy > 60) {
                target_height *= 1.1f;
            }
            rhythm_jump_state.segments[i].stiffness = 0.45f;
        }
        
        // 全局能量增强
        target_height *= (1.0f + global_energy * 0.01f);
        
        // 确保最小高度（压得很低，安静时接近熄灭，突出鼓点对比）
        if (target_height < 0.04f) target_height = 0.04f;
        
        rhythm_jump_state.segments[i].target_height = target_height + rj_pulse * 1.2f;
    }
    
    update_physics_simulation();
    
    // 颜色变化
    rhythm_jump_state.color_hue += rhythm_jump_state.hue_speed * g_fx.color_speed;
    if (rhythm_jump_state.color_hue > 1.0f) {
        rhythm_jump_state.color_hue -= 1.0f;
    }
    
    int leds_per_segment = led_count / 8;
    int extra_leds = led_count % 8;
    
    // 计算全局亮度增强
    float global_brightness = 1.0f + global_energy * 0.02f;
    if (global_brightness > 1.5f) global_brightness = 1.5f;
    
    for (int segment = 0; segment < 8; segment++) {
        int start_led = segment * leds_per_segment;
        if (segment < extra_leds) {
            start_led += segment;
        } else {
            start_led += extra_leds;
        }
        
        int segment_leds = leds_per_segment;
        if (segment < extra_leds) {
            segment_leds++;
        }
        
        float height = rhythm_jump_state.segments[segment].height;
        int lit_leds = (int)(height * segment_leds);
        if (lit_leds > segment_leds) lit_leds = segment_leds;
        
        // 动态颜色：根据高度变化色调和饱和度（脉冲时更白更亮）
        float hue = fmodf(rhythm_jump_state.color_hue + (segment * 0.125f), 1.0f);
        float saturation = 1.0f - height * 0.2f - rj_pulse * 0.4f;  // 高度越高、脉冲越强越白
        if (saturation < 0.5f) saturation = 0.5f;
        
        // 动态亮度：底色很暗，高度/鼓点越强才越亮，对比拉满
        float value = (0.06f + height * 0.95f) * global_brightness *
                      (rhythm_jump_state.brightness_boost + 0.7f * rj_pulse);
        if (value > 1.0f) value = 1.0f;
        
        rgb_color_t segment_color = hsv_to_rgb(hue, saturation, value);
        
        // 增强的发光效果：不仅仅是点亮，还有渐变
        for (int i = 0; i < segment_leds; i++) {
            int led_index = start_led + i;
            if (led_index >= led_count) break;
            
            if (i < lit_leds) {
                // 使用更明显的渐变：底部到顶部
                float position_factor = (float)i / lit_leds;
                
                // 非线性渐变，让顶部更亮
                float led_brightness = 0.4f + powf(position_factor, 1.5f) * 0.6f;
                
                // 添加一些颜色变化：从底部的暖色到顶部的冷色
                float color_shift = position_factor * 0.1f;
                rgb_color_t led_color = segment_color;
                
                // 调整颜色
                led_color.r = (uint8_t)(segment_color.r * led_brightness);
                led_color.g = (uint8_t)(segment_color.g * led_brightness * (1.0f - color_shift));
                led_color.b = (uint8_t)(segment_color.b * led_brightness * (1.0f + color_shift));
                
                set_buffer_pixel(led_index, led_color);
                
                // 增强光晕效果
                if (led_brightness > 0.3f) {
                    float glow_intensity = led_brightness * 0.4f;  // 增强光晕强度
                    rgb_color_t glow_color = {
                        .r = (uint8_t)(segment_color.r * glow_intensity),
                        .g = (uint8_t)(segment_color.g * glow_intensity),
                        .b = (uint8_t)(segment_color.b * glow_intensity)
                    };
                    
                    // 向两侧扩散光晕
                    for (int offset = 1; offset <= 2; offset++) {
                        if (led_index - offset >= 0) {
                            float distance_factor = 1.0f - (offset * 0.3f);
                            set_buffer_pixel_blend(led_index - offset, glow_color, 0.4f * distance_factor);
                        }
                        if (led_index + offset < led_count) {
                            float distance_factor = 1.0f - (offset * 0.3f);
                            set_buffer_pixel_blend(led_index + offset, glow_color, 0.4f * distance_factor);
                        }
                    }
                }
            } else {
                // 背景光：很暗的余光，安静时接近黑，突出鼓点
                float background_brightness = 0.05f + height * 0.04f;
                rgb_color_t bg_color = {
                    .r = (uint8_t)(segment_color.r * background_brightness * 0.25f),
                    .g = (uint8_t)(segment_color.g * background_brightness * 0.25f),
                    .b = (uint8_t)(segment_color.b * background_brightness * 0.25f)
                };
                
                set_buffer_pixel(led_index, bg_color);
            }
        }
        
        // 顶部特效：当跳动到一定高度时
        if (height > 0.2f && lit_leds > 0) {
            int top_led = start_led + lit_leds - 1;
            if (top_led < led_count) {
                // 顶部高亮光点
                float top_brightness = 0.8f + height * 0.4f;
                if (top_brightness > 1.0f) top_brightness = 1.0f;
                
                rgb_color_t top_color = {
                    .r = (uint8_t)(255 * top_brightness),
                    .g = (uint8_t)(255 * top_brightness * 0.9f),
                    .b = (uint8_t)(200 * top_brightness)
                };
                set_buffer_pixel_blend(top_led, top_color, 0.9f);
                
                // 顶部粒子效果
                if (height > 0.5f && (animation_counter + segment) % 3 == 0) {
                    for (int p = 0; p < 2; p++) {
                        int particle_pos = top_led + (rand() % 5) - 2;
                        if (particle_pos >= 0 && particle_pos < led_count) {
                            float particle_brightness = 0.5f + (rand() % 50) / 100.0f;
                            float particle_hue = fmodf(rhythm_jump_state.color_hue + 0.3f, 1.0f);
                            rgb_color_t particle_color = hsv_to_rgb(particle_hue, 0.8f, particle_brightness);
                            set_buffer_pixel_blend(particle_pos, particle_color, 0.7f);
                        }
                    }
                }
            }
        }
        
        // 底部发光：模拟地面反光
        if (height > 0.3f) {
            int bottom_led = start_led;
            if (bottom_led < led_count) {
                float ground_glow = height * 0.2f;
                rgb_color_t ground_color = {
                    .r = (uint8_t)(segment_color.r * ground_glow),
                    .g = (uint8_t)(segment_color.g * ground_glow),
                    .b = (uint8_t)(segment_color.b * ground_glow)
                };
                set_buffer_pixel_blend(bottom_led, ground_color, 0.4f);
            }
        }
    }
    
    // 添加全局效果：根据全局能量添加闪烁
    if (global_energy > 30 && (animation_counter % 10) == 0) {
        float flash_intensity = fminf(global_energy / 100.0f, 0.3f);
        rgb_color_t flash_color = {255, 255, 200};
        
        // 在随机位置添加闪烁点
        for (int f = 0; f < 3; f++) {
            int flash_pos = rand() % led_count;
            set_buffer_pixel_blend(flash_pos, flash_color, flash_intensity);
        }
    }
    
    // 分段分隔线：更柔和
    for (int segment = 1; segment < 8; segment++) {
        int divider_led = (segment * leds_per_segment) + 
                         (segment < extra_leds ? segment : extra_leds) - 1;
        if (divider_led >= 0 && divider_led < led_count) {
            float divider_brightness = 0.1f + global_energy * 0.005f;
            rgb_color_t divider_color = {
                .r = (uint8_t)(40 * divider_brightness),
                .g = (uint8_t)(40 * divider_brightness),
                .b = (uint8_t)(60 * divider_brightness)
            };
            set_buffer_pixel_blend(divider_led, divider_color, 0.2f);
        }
    }
    
    return apply_color_buffer();
}

// 初始化闪烁数组
static void init_sparkles(void) {
    for (int i = 0; i < 15; i++) {
        sparkle_rainbow_state.sparkles[i].active = false;
    }
}

// 创建一个新的闪烁
static void create_sparkle(float position, int direction, rgb_color_t color, float speed, int size) {
    for (int i = 0; i < 15; i++) {
        if (!sparkle_rainbow_state.sparkles[i].active) {
            sparkle_rainbow_state.sparkles[i].active = true;
            sparkle_rainbow_state.sparkles[i].position = position;
            sparkle_rainbow_state.sparkles[i].direction = direction;
            sparkle_rainbow_state.sparkles[i].color = color;
            sparkle_rainbow_state.sparkles[i].speed = speed;
            sparkle_rainbow_state.sparkles[i].size = size;
            sparkle_rainbow_state.sparkles[i].brightness = 1.0f;
            sparkle_rainbow_state.sparkles[i].flash_speed = 0.1f + ((float)rand() / RAND_MAX) * 0.2f;
            sparkle_rainbow_state.sparkles[i].lifetime = 300;
            break;
        }
    }
}

// 根据频率能量选择闪烁颜色
static rgb_color_t get_energy_color(float bass_energy, float mid_energy, float high_energy) {
    float total_energy = bass_energy + mid_energy + high_energy;
    
    if (total_energy < 0.1f) {
        float hue = fmodf(sparkle_rainbow_state.hue_base, 1.0f);
        return hsv_to_rgb(hue, 0.9f, 1.0f);
    }
    
    if (bass_energy >= mid_energy && bass_energy >= high_energy) {
        float hue = 0.0f + ((float)rand() / RAND_MAX) * 0.1f;
        return hsv_to_rgb(hue, 0.9f, 1.0f);
    } else if (mid_energy >= bass_energy && mid_energy >= high_energy) {
        float hue = 0.3f + ((float)rand() / RAND_MAX) * 0.1f;
        return hsv_to_rgb(hue, 0.9f, 1.0f);
    } else {
        float hue = 0.6f + ((float)rand() / RAND_MAX) * 0.2f;
        return hsv_to_rgb(hue, 0.9f, 1.0f);
    }
}

// 闪烁彩虹效果实现
static esp_err_t sparkle_rainbow_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    clear_color_buffer();
    
    static bool initialized = false;
    if (!initialized) {
        init_sparkles();
        initialized = true;
    }
    
    float bass_energy = 0, mid_energy = 0, high_energy = 0, total_energy = 0;
    
    if (energy_bands && num_bands > 0) {
        for (int i = 0; i < num_bands; i++) {
            float energy = energy_bands[i];
            total_energy += energy;
            
            if (i < num_bands / 3) {
                bass_energy += energy;
            } else if (i < num_bands * 2 / 3) {
                mid_energy += energy;
            } else {
                high_energy += energy;
            }
        }
    }
    
    sparkle_rainbow_state.hue_base += sparkle_rainbow_state.hue_speed * g_fx.color_speed;
    if (sparkle_rainbow_state.hue_base > 1.0f) {
        sparkle_rainbow_state.hue_base -= 1.0f;
    }
    
    sparkle_rainbow_state.rainbow_offset += sparkle_rainbow_state.rainbow_speed * g_fx.speed;
    if (sparkle_rainbow_state.rainbow_offset > 1.0f) {
        sparkle_rainbow_state.rainbow_offset -= 1.0f;
    }
    
    for (int i = 0; i < led_count; i++) {
        float pos = (float)i / led_count;
        float hue = fmodf(pos + sparkle_rainbow_state.rainbow_offset, 1.0f);
        
        float base_brightness = 0.3f;
        float energy_boost = fminf(total_energy / 100.0f, 0.4f);
        float brightness = base_brightness + energy_boost;
        
        rgb_color_t rainbow_color = hsv_to_rgb(hue, 0.9f, brightness);
        set_buffer_pixel(i, rainbow_color);
    }
    
    uint32_t current_time = animation_counter;
    bool should_emit = false;
    
    if (total_energy > sparkle_rainbow_state.energy_threshold) {
        if (total_energy > sparkle_rainbow_state.last_energy * 1.2f) {
            should_emit = true;
        }
        
        float energy_factor = fminf(total_energy / 80.0f, 1.0f);
        float adjusted_interval = sparkle_rainbow_state.emit_interval * (2.0f - energy_factor);
        
        if (current_time - sparkle_rainbow_state.last_emit_time > adjusted_interval) {
            should_emit = true;
        }
    }
    
    sparkle_rainbow_state.last_energy = total_energy;
    
    if (should_emit) {
        sparkle_rainbow_state.last_emit_time = current_time;
        
        int direction;
        if (sparkle_rainbow_state.emit_direction == 0) {
            direction = (rand() % 2 == 0) ? 1 : -1;
        } else if (sparkle_rainbow_state.emit_direction == 1) {
            direction = sparkle_rainbow_state.next_direction;
            sparkle_rainbow_state.next_direction = -sparkle_rainbow_state.next_direction;
        } else {
            direction = sparkle_rainbow_state.emit_direction;
        }
        
        float start_position;
        if (direction == 1) {
            start_position = -3.0f;
        } else {
            start_position = led_count + 3.0f;
        }
        
        int size = 1 + (int)(fminf(total_energy / 40.0f, 1.0f) * 4);
        
        rgb_color_t sparkle_color = get_energy_color(bass_energy, mid_energy, high_energy);
        
        float base_speed = 0.333f;
        float speed_factor = 1.0f + fminf(total_energy / 120.0f, 0.5f);
        float speed = base_speed * speed_factor * direction;
        
        create_sparkle(start_position, direction, sparkle_color, speed, size);
        
        if (total_energy > 50.0f && (rand() % 3 == 0)) {
            float offset = (direction == 1) ? -2.0f : 2.0f;
            create_sparkle(start_position + offset, direction, 
                          get_energy_color(bass_energy, mid_energy, high_energy), 
                          speed * 0.8f, size - 1);
        }
    }
    
    for (int i = 0; i < 15; i++) {
        sparkle_t *sparkle = &sparkle_rainbow_state.sparkles[i];
        
        if (!sparkle->active) continue;
        
        sparkle->position += sparkle->speed * g_fx.speed;
        sparkle->brightness = 0.7f + sinf(current_time * sparkle->flash_speed) * 0.3f;
        sparkle->lifetime--;
        
        if (sparkle->brightness > 0.1f) {
            for (int j = 0; j < sparkle->size; j++) {
                int led_pos;
                if (sparkle->direction == 1) {
                    led_pos = (int)(sparkle->position - j);
                } else {
                    led_pos = (int)(sparkle->position + j);
                }
                
                if (led_pos >= 0 && led_pos < led_count) {
                    float position_factor;
                    if (sparkle->size == 1) {
                        position_factor = 1.0f;
                    } else {
                        position_factor = 1.0f - (float)j / sparkle->size * 0.5f;
                    }
                    
                    float led_brightness = sparkle->brightness * position_factor;
                    
                    rgb_color_t led_color = {
                        .r = (uint8_t)(sparkle->color.r * led_brightness),
                        .g = (uint8_t)(sparkle->color.g * led_brightness),
                        .b = (uint8_t)(sparkle->color.b * led_brightness)
                    };
                    
                    set_buffer_pixel_blend(led_pos, led_color, 1.0f);
                    
                    if (led_brightness > 0.5f) {
                        rgb_color_t glow_color = {
                            .r = (uint8_t)(sparkle->color.r * led_brightness * 0.3f),
                            .g = (uint8_t)(sparkle->color.g * led_brightness * 0.3f),
                            .b = (uint8_t)(sparkle->color.b * led_brightness * 0.3f)
                        };
                        
                        if (led_pos > 0) {
                            set_buffer_pixel_blend(led_pos - 1, glow_color, 0.4f);
                        }
                        if (led_pos < led_count - 1) {
                            set_buffer_pixel_blend(led_pos + 1, glow_color, 0.4f);
                        }
                    }
                }
            }
        }
        
        if (sparkle->lifetime <= 0 || 
            (sparkle->direction == 1 && sparkle->position > led_count + 5) ||
            (sparkle->direction == -1 && sparkle->position < -5)) {
            sparkle->active = false;
        }
    }
    
    if (total_energy > 20.0f) {
        float background_intensity = fminf(total_energy / 150.0f, 0.3f);
        
        if (background_intensity > 0.01f) {
            if (bass_energy > mid_energy && bass_energy > high_energy) {
                for (int i = 0; i < led_count; i++) {
                    float pos_factor = (float)i / led_count;
                    float intensity = background_intensity * (1.0f - pos_factor * 0.5f);
                    
                    rgb_color_t bg_color = {
                        .r = (uint8_t)(intensity * 150),
                        .g = (uint8_t)(intensity * 50),
                        .b = (uint8_t)(intensity * 20)
                    };
                    
                    set_buffer_pixel_blend(i, bg_color, 0.3f);
                }
            } else if (high_energy > bass_energy && high_energy > mid_energy) {
                for (int i = 0; i < led_count; i++) {
                    float pos_factor = (float)i / led_count;
                    float intensity = background_intensity * pos_factor;
                    
                    rgb_color_t bg_color = {
                        .r = (uint8_t)(intensity * 30),
                        .g = (uint8_t)(intensity * 60),
                        .b = (uint8_t)(intensity * 150)
                    };
                    
                    set_buffer_pixel_blend(i, bg_color, 0.3f);
                }
            }
        }
    }
    
    return apply_color_buffer();
}
// 初始化离子粒子
/*static void init_ion_particle(ion_particle_t *ion, int direction, rgb_color_t color) {
    if (!ion) return;
    
    ion->active = true;
    ion->direction = direction;
    ion->color = color;
    ion->brightness = 1.0f;
    ion->trail_length = 3.0f;
    ion->size = 2.0f;
    ion->spawn_time = animation_counter;
    
    if (direction == 1) {
        ion->position = -5.0f;
        ion->velocity = 0.5f;
    } else {
        ion->position = led_count + 5.0f;
        ion->velocity = -0.5f;
    }
}*/
// 初始化爆炸效果
static void init_explosion_particle(int index, float position, rgb_color_t color, float intensity, int direction) {
    if (index < 0 || index >= MAX_PARTICLES) return;
    
    explosion_collision_state.particles[index].active = true;
    explosion_collision_state.particles[index].position = position;
    explosion_collision_state.particles[index].color = color;
    
    // 随机速度方向
    float base_speed = 0.5f + intensity * 1.5f;
    if (direction == 0) {
        explosion_collision_state.particles[index].direction = (rand() % 3) - 1; // -1, 0, 1
    } else {
        explosion_collision_state.particles[index].direction = direction;
    }
    
    // 随机速度
    explosion_collision_state.particles[index].velocity = 
        base_speed * (0.5f + (rand() % 100) / 100.0f);
    
    // 随机加速度
    explosion_collision_state.particles[index].acceleration = 
        -0.02f - (rand() % 100) / 1000.0f;
    
    // 随机大小
    explosion_collision_state.particles[index].size = 
        0.5f + (rand() % 100) / 200.0f;
    
    // 随机亮度参数
    explosion_collision_state.particles[index].max_brightness = 
        0.8f + (rand() % 40) / 100.0f;
    explosion_collision_state.particles[index].brightness = 
        explosion_collision_state.particles[index].max_brightness;
    explosion_collision_state.particles[index].brightness_speed = 
        0.02f + (rand() % 30) / 1000.0f;
    explosion_collision_state.particles[index].flicker_speed = 
        0.05f + (rand() % 50) / 1000.0f;
    explosion_collision_state.particles[index].flicker_phase = 
        (rand() % 314) / 100.0f; // 0-3.14
    
    // 物理参数
    explosion_collision_state.particles[index].gravity = 0.02f;
    explosion_collision_state.particles[index].friction = 0.98f;
    
    // 寿命
    explosion_collision_state.particles[index].lifespan = 
        30.0f + intensity * 60.0f + (rand() % 60);
    explosion_collision_state.particles[index].age = 0;
}

// 创建爆炸粒子群
static void create_explosion_particles(float position, rgb_color_t core_color, float intensity) {
    int particles_to_create = (int)(intensity * 60);
    if (particles_to_create > MAX_PARTICLES) particles_to_create = MAX_PARTICLES;
    
    int created = 0;
    for (int i = 0; i < MAX_PARTICLES && created < particles_to_create; i++) {
        if (!explosion_collision_state.particles[i].active) {
            // 随机颜色变化
            float hue_shift = ((rand() % 60) - 30) / 360.0f;
            rgb_color_t particle_color = hsv_to_rgb(
                fmodf(rgb_to_hue(core_color) + hue_shift, 1.0f),
                0.7f + (rand() % 30) / 100.0f,
                1.0f
            );
            
            init_explosion_particle(i, position, particle_color, intensity, 0);
            created++;
        }
    }
}

// RGB转HSV的辅助函数
static float rgb_to_hue(rgb_color_t color) {
    float r = color.r / 255.0f;
    float g = color.g / 255.0f;
    float b = color.b / 255.0f;
    
    float max = fmaxf(r, fmaxf(g, b));
    float min = fminf(r, fminf(g, b));
    
    if (max == min) return 0.0f;
    
    float hue;
    if (max == r) {
        hue = (g - b) / (max - min);
    } else if (max == g) {
        hue = 2.0f + (b - r) / (max - min);
    } else {
        hue = 4.0f + (r - g) / (max - min);
    }
    
    hue *= 60.0f;
    if (hue < 0) hue += 360.0f;
    
    return hue / 360.0f;
}
// 初始化离子粒子
static void init_ion_particle(ion_particle_t *ion, int direction, rgb_color_t color) {
    if (!ion) return;
    
    ion->active = true;
    ion->direction = direction;
    ion->color = color;
    ion->brightness = 1.0f;
    ion->trail_length = 4.0f;  // 增加拖尾长度
    ion->size = 2.5f;          // 增加离子大小
    ion->spawn_time = animation_counter;
    
    if (direction == 1) {
        ion->position = -8.0f;  // 从更远的地方开始
        ion->velocity = 0.8f;   // 增加速度
    } else {
        ion->position = led_count + 8.0f;
        ion->velocity = -0.8f;
    }
}

// 初始化爆炸效果
static void init_explosion(float position, float intensity, rgb_color_t core_color) {
    explosion_collision_state.explosion.active = true;
    explosion_collision_state.explosion.position = position;
    explosion_collision_state.explosion.radius = 0.0f;
    explosion_collision_state.explosion.max_radius = 12.0f + intensity * 10.0f; // 增加爆炸半径
    explosion_collision_state.explosion.intensity = intensity;
    explosion_collision_state.explosion.decay_rate = 0.85f; // 减慢衰减
    explosion_collision_state.explosion.core_color = core_color;
    
    // 创建爆炸粒子
    create_explosion_particles(position, core_color, intensity);
    
    explosion_collision_state.explosion.shockwave_color.r = 
        (uint8_t)(core_color.r * 0.9f > 255 ? 255 : core_color.r * 0.9f);
    explosion_collision_state.explosion.shockwave_color.g = 
        (uint8_t)(core_color.g * 0.9f > 255 ? 255 : core_color.g * 0.9f);
    explosion_collision_state.explosion.shockwave_color.b = 
        (uint8_t)(core_color.b * 0.9f > 255 ? 255 : core_color.b * 0.9f);
    
    explosion_collision_state.explosion.start_time = animation_counter;
    explosion_collision_state.explosion.duration = (uint32_t)(10 + intensity * 8); // 缩短持续时间
}

static rgb_color_t get_ion_color_from_energy(float bass_energy, float mid_energy, float high_energy, int direction) {
    if (direction == 1) {
        explosion_collision_state.hue_left += explosion_collision_state.hue_speed * g_fx.color_speed;
        if (explosion_collision_state.hue_left > 1.0f) {
            explosion_collision_state.hue_left -= 1.0f;
        }
        
        float hue = explosion_collision_state.hue_left;
        
        // 根据能量类型调整颜色
        if (bass_energy > mid_energy && bass_energy > high_energy) {
            hue = fmodf(hue + 0.05f, 1.0f);  // 低音时偏向红色/橙色
        } else if (mid_energy > bass_energy && mid_energy > high_energy) {
            hue = fmodf(hue + 0.35f, 1.0f);  // 中音时偏向绿色/青色
        } else {
            hue = fmodf(hue + 0.65f, 1.0f);  // 高音时偏向蓝色/紫色
        }
        
        float saturation = 0.95f;
        float value = 1.0f;
        
        return hsv_to_rgb(hue, saturation, value);
    } else {
        explosion_collision_state.hue_right += explosion_collision_state.hue_speed * g_fx.color_speed;
        if (explosion_collision_state.hue_right > 1.0f) {
            explosion_collision_state.hue_right -= 1.0f;
        }
        
        float hue = explosion_collision_state.hue_right;
        
        // 根据能量类型调整颜色
        if (bass_energy > mid_energy && bass_energy > high_energy) {
            hue = fmodf(hue + 0.02f, 1.0f);
        } else if (mid_energy > bass_energy && mid_energy > high_energy) {
            hue = fmodf(hue + 0.38f, 1.0f);
        } else {
            hue = fmodf(hue + 0.70f, 1.0f);
        }
        
        float saturation = 0.95f;
        float value = 1.0f;
        
        return hsv_to_rgb(hue, saturation, value);
    }
}
// 混合离子颜色以产生碰撞效果
static rgb_color_t mix_ion_colors(rgb_color_t color_left, rgb_color_t color_right) {
    rgb_color_t mixed;
    
    // 使用加权混合而不是简单平均
    mixed.r = (uint8_t)((color_left.r * 0.6f + color_right.r * 0.4f));
    mixed.g = (uint8_t)((color_left.g * 0.6f + color_right.g * 0.4f));
    mixed.b = (uint8_t)((color_left.b * 0.6f + color_right.b * 0.4f));
    
    // 增加亮度
    mixed.r = mixed.r < 200 ? mixed.r + 55 : 255;
    mixed.g = mixed.g < 200 ? mixed.g + 55 : 255;
    mixed.b = mixed.b < 200 ? mixed.b + 55 : 255;
    
    return mixed;
}

static bool check_collision(void) {
    if (!explosion_collision_state.left_ion.active || !explosion_collision_state.right_ion.active) {
        return false;
    }
    
    float left_pos = explosion_collision_state.left_ion.position;
    float right_pos = explosion_collision_state.right_ion.position;
    
    float collision_start = led_count * explosion_collision_state.collision_zone_start;
    float collision_end = led_count * explosion_collision_state.collision_zone_end;
    
    bool left_in_zone = (left_pos >= collision_start && left_pos <= collision_end);
    bool right_in_zone = (right_pos >= collision_start && right_pos <= collision_end);
    
    // 简化碰撞检测：只要在碰撞区域内相遇
    if (left_in_zone && right_in_zone && fabsf(left_pos - right_pos) < 10.0f) {
        return true;
    }
    
    // 或者距离很近
    float distance = fabsf(left_pos - right_pos);
    if (distance < 5.0f) {
        return true;
    }
    
    return false;
}

// 更新爆炸粒子
static void update_explosion_particles(void) {
    explosion_collision_state.particle_timer++;
    
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!explosion_collision_state.particles[i].active) continue;
        
        explosion_particle_t *p = &explosion_collision_state.particles[i];
        
        // 更新年龄
        p->age++;
        
        // 如果粒子寿命结束，标记为非活跃
        if (p->age > p->lifespan) {
            p->active = false;
            continue;
        }
        
        // 更新位置
        if (p->velocity != 0) {
            // 应用加速度
            p->velocity += p->acceleration;
            
            // 应用摩擦力
            p->velocity *= p->friction;
            
            // 应用重力（如果方向向下）
            if (p->direction == 0) {
                p->velocity -= p->gravity;
            }
            
            // 更新位置
            p->position += p->velocity * (float)p->direction * g_fx.speed;
            
            // 边界检查
            if (p->position < 0 || p->position >= led_count) {
                // 碰到边界后反弹或停止
                if (p->position < 0) {
                    p->position = 0;
                    p->direction = 1;
                } else if (p->position >= led_count) {
                    p->position = led_count - 1;
                    p->direction = -1;
                }
                p->velocity *= 0.3f; // 反弹损失能量
            }
            
            // 如果速度很小，就停止运动
            if (fabsf(p->velocity) < 0.05f) {
                p->velocity = 0;
            }
        }
        
        // 更新闪烁效果
        p->flicker_phase += p->flicker_speed;
        if (p->flicker_phase > 2 * M_PI) {
            p->flicker_phase -= 2 * M_PI;
        }
        
        // 计算闪烁亮度
        float flicker = 0.5f + 0.5f * sinf(p->flicker_phase);
        
        // 随着年龄衰减
        float age_factor = 1.0f - (p->age / p->lifespan);
        
        // 计算最终亮度
        p->brightness = p->max_brightness * flicker * age_factor;
        
        // 偶尔随机变化一下闪烁速度
        if (explosion_collision_state.particle_timer % 30 == 0 && rand() % 100 < 10) {
            p->flicker_speed = 0.03f + (rand() % 40) / 1000.0f;
        }
    }
}

static esp_err_t explosion_collision_effect(float *energy_bands, int num_bands) {
    if (!strip) return ESP_ERR_INVALID_STATE;
    
    clear_color_buffer();
    
    float bass_energy = 0, mid_energy = 0, high_energy = 0, total_energy = 0;
    
    if (energy_bands && num_bands > 0) {
        for (int i = 0; i < num_bands; i++) {
            float energy = energy_bands[i];
            total_energy += energy;
            
            if (i < num_bands / 3) {
                bass_energy += energy;
            } else if (i < num_bands * 2 / 3) {
                mid_energy += energy;
            } else {
                high_energy += energy;
            }
        }
        
        // 归一化
        if (num_bands > 0) {
            bass_energy /= num_bands / 3;
            mid_energy /= num_bands / 3;
            high_energy /= num_bands - (num_bands * 2 / 3);
            total_energy /= num_bands;
        }
    }
    
    explosion_collision_state.state_timer++;
    
    switch (explosion_collision_state.state) {
        case STATE_IDLE: {
            bool should_launch = false;
            uint32_t current_time = animation_counter;
            
            // 基于能量的发射条件
            if (total_energy > explosion_collision_state.energy_threshold) {
                // 能量突然增加时发射
                if (total_energy > explosion_collision_state.last_energy * 1.3f) {
                    should_launch = true;
                }
                
                // 高能量时发射概率增加
                if (total_energy > 50.0f && rand() % 100 < 30) {
                    should_launch = true;
                }
            }
            
            // 时间间隔发射
            if (current_time - explosion_collision_state.last_launch_time > 
                explosion_collision_state.launch_interval) {
                should_launch = true;
            }
            
            if (should_launch) {
                rgb_color_t left_color = get_ion_color_from_energy(
                    bass_energy, mid_energy, high_energy, 1);
                rgb_color_t right_color = get_ion_color_from_energy(
                    bass_energy, mid_energy, high_energy, -1);
                
                init_ion_particle(&explosion_collision_state.left_ion, 1, left_color);
                init_ion_particle(&explosion_collision_state.right_ion, -1, right_color);
                
                explosion_collision_state.last_launch_time = current_time;
                explosion_collision_state.state = STATE_ION_FLYING;
                explosion_collision_state.state_timer = 0;
            }
            
            break;
        }
        
        case STATE_ION_FLYING: {
            // 更新离子位置
            if (explosion_collision_state.left_ion.active) {
                explosion_collision_state.left_ion.position += 
                    explosion_collision_state.left_ion.velocity * g_fx.speed;
                
                // 添加一些动态效果：速度随能量变化
                explosion_collision_state.left_ion.velocity *= 1.001f;
                
                if (explosion_collision_state.left_ion.position > led_count + 20) {
                    explosion_collision_state.left_ion.active = false;
                }
            }
            
            if (explosion_collision_state.right_ion.active) {
                explosion_collision_state.right_ion.position += 
                    explosion_collision_state.right_ion.velocity * g_fx.speed;
                
                explosion_collision_state.right_ion.velocity *= 1.001f;
                
                if (explosion_collision_state.right_ion.position < -20) {
                    explosion_collision_state.right_ion.active = false;
                }
            }
            
            // 检查碰撞
            if (check_collision()) {
                float collision_pos = (explosion_collision_state.left_ion.position + 
                                     explosion_collision_state.right_ion.position) * 0.5f;
                
                // 确保碰撞位置在灯带范围内
                if (collision_pos < 0) collision_pos = 0;
                if (collision_pos >= led_count) collision_pos = led_count - 1;
                
                // 计算碰撞强度
                float collision_speed = fabsf(explosion_collision_state.left_ion.velocity) +
                                      fabsf(explosion_collision_state.right_ion.velocity);
                float collision_intensity = fminf(collision_speed * 1.5f, 1.5f);
                collision_intensity = fmaxf(collision_intensity, total_energy / 50.0f);
                
                rgb_color_t explosion_color = mix_ion_colors(
                    explosion_collision_state.left_ion.color,
                    explosion_collision_state.right_ion.color);
                
                init_explosion(collision_pos, collision_intensity, explosion_color);
                
                explosion_collision_state.left_ion.active = false;
                explosion_collision_state.right_ion.active = false;
                
                explosion_collision_state.state = STATE_EXPLODING;
                explosion_collision_state.state_timer = 0;
                explosion_collision_state.collision_count++;
            }
            
            // 超时处理
            if (explosion_collision_state.state_timer > 150) {
                explosion_collision_state.left_ion.active = false;
                explosion_collision_state.right_ion.active = false;
                explosion_collision_state.state = STATE_IDLE;
                explosion_collision_state.state_timer = 0;
            }
            
            break;
        }
        
        case STATE_EXPLODING: {
            if (!explosion_collision_state.explosion.active) {
                explosion_collision_state.state = STATE_PARTICLE_PHASE;
                explosion_collision_state.state_timer = 0;
                break;
            }
            
            // 更新爆炸效果
            explosion_collision_state.explosion.radius += 1.2f;
            explosion_collision_state.explosion.intensity *= 
                explosion_collision_state.explosion.decay_rate;
            
            uint32_t explosion_age = animation_counter - 
                                   explosion_collision_state.explosion.start_time;
            
            // 爆炸持续时间缩短，更快进入粒子阶段
            if (explosion_collision_state.explosion.radius > 
                explosion_collision_state.explosion.max_radius * 0.7f ||
                explosion_age > explosion_collision_state.explosion.duration ||
                explosion_collision_state.explosion.intensity < 0.1f) {
                
                explosion_collision_state.explosion.active = false;
                explosion_collision_state.state = STATE_PARTICLE_PHASE;
                explosion_collision_state.state_timer = 0;
            }
            
            break;
        }
        
        case STATE_PARTICLE_PHASE: {
            // 粒子阶段持续时间
            if (explosion_collision_state.state_timer > 120) {
                // 检查是否还有活跃的粒子
                bool any_particles_active = false;
                for (int i = 0; i < MAX_PARTICLES; i++) {
                    if (explosion_collision_state.particles[i].active) {
                        any_particles_active = true;
                        break;
                    }
                }
                
                if (!any_particles_active) {
                    explosion_collision_state.state = STATE_IDLE;
                    explosion_collision_state.state_timer = 0;
                }
            }
            break;
        }
    }
    
    // 更新粒子
    update_explosion_particles();
    
    explosion_collision_state.last_energy = total_energy;
    
    // 绘制离子
    if (explosion_collision_state.state == STATE_ION_FLYING) {
        if (explosion_collision_state.left_ion.active) {
            float ion_brightness = explosion_collision_state.left_ion.brightness;
            float pos = explosion_collision_state.left_ion.position;
            
            int core_pos = (int)pos;
            if (core_pos >= 0 && core_pos < led_count) {
                rgb_color_t core_color = {
                    .r = (uint8_t)(explosion_collision_state.left_ion.color.r * ion_brightness),
                    .g = (uint8_t)(explosion_collision_state.left_ion.color.g * ion_brightness),
                    .b = (uint8_t)(explosion_collision_state.left_ion.color.b * ion_brightness)
                };
                
                // 增强离子核心亮度
                set_buffer_pixel(core_pos, core_color);
                
                // 增强光晕效果
                float glow_radius = explosion_collision_state.left_ion.size;
                for (int offset = 1; offset <= (int)glow_radius; offset++) {
                    float glow_intensity = ion_brightness * (0.6f / (offset * 0.7f));
                    rgb_color_t glow_color = {
                        .r = (uint8_t)(core_color.r * glow_intensity),
                        .g = (uint8_t)(core_color.g * glow_intensity),
                        .b = (uint8_t)(core_color.b * glow_intensity)
                    };
                    
                    if (core_pos - offset >= 0) {
                        set_buffer_pixel_blend(core_pos - offset, glow_color, 0.7f);
                    }
                    if (core_pos + offset < led_count) {
                        set_buffer_pixel_blend(core_pos + offset, glow_color, 0.7f);
                    }
                }
            }
            
            // 增强拖尾效果
            float trail_length = explosion_collision_state.left_ion.trail_length;
            for (int i = 1; i <= (int)trail_length; i++) {
                int trail_pos = core_pos - i;
                if (trail_pos >= 0 && trail_pos < led_count) {
                    float trail_intensity = ion_brightness * (0.5f / (i * 0.8f));
                    rgb_color_t trail_color = {
                        .r = (uint8_t)(explosion_collision_state.left_ion.color.r * trail_intensity),
                        .g = (uint8_t)(explosion_collision_state.left_ion.color.g * trail_intensity),
                        .b = (uint8_t)(explosion_collision_state.left_ion.color.b * trail_intensity)
                    };
                    
                    // 拖尾也有光晕
                    set_buffer_pixel_blend(trail_pos, trail_color, 0.5f);
                    
                    // 拖尾光晕
                    if (trail_intensity > 0.2f) {
                        rgb_color_t trail_glow = {
                            .r = (uint8_t)(trail_color.r * 0.4f),
                            .g = (uint8_t)(trail_color.g * 0.4f),
                            .b = (uint8_t)(trail_color.b * 0.4f)
                        };
                        
                        if (trail_pos - 1 >= 0) {
                            set_buffer_pixel_blend(trail_pos - 1, trail_glow, 0.3f);
                        }
                        if (trail_pos + 1 < led_count) {
                            set_buffer_pixel_blend(trail_pos + 1, trail_glow, 0.3f);
                        }
                    }
                }
            }
        }
        
        if (explosion_collision_state.right_ion.active) {
            float ion_brightness = explosion_collision_state.right_ion.brightness;
            float pos = explosion_collision_state.right_ion.position;
            
            int core_pos = (int)pos;
            if (core_pos >= 0 && core_pos < led_count) {
                rgb_color_t core_color = {
                    .r = (uint8_t)(explosion_collision_state.right_ion.color.r * ion_brightness),
                    .g = (uint8_t)(explosion_collision_state.right_ion.color.g * ion_brightness),
                    .b = (uint8_t)(explosion_collision_state.right_ion.color.b * ion_brightness)
                };
                
                set_buffer_pixel(core_pos, core_color);
                
                float glow_radius = explosion_collision_state.right_ion.size;
                for (int offset = 1; offset <= (int)glow_radius; offset++) {
                    float glow_intensity = ion_brightness * (0.6f / (offset * 0.7f));
                    rgb_color_t glow_color = {
                        .r = (uint8_t)(core_color.r * glow_intensity),
                        .g = (uint8_t)(core_color.g * glow_intensity),
                        .b = (uint8_t)(core_color.b * glow_intensity)
                    };
                    
                    if (core_pos - offset >= 0) {
                        set_buffer_pixel_blend(core_pos - offset, glow_color, 0.7f);
                    }
                    if (core_pos + offset < led_count) {
                        set_buffer_pixel_blend(core_pos + offset, glow_color, 0.7f);
                    }
                }
            }
            
            float trail_length = explosion_collision_state.right_ion.trail_length;
            for (int i = 1; i <= (int)trail_length; i++) {
                int trail_pos = core_pos + i;
                if (trail_pos >= 0 && trail_pos < led_count) {
                    float trail_intensity = ion_brightness * (0.5f / (i * 0.8f));
                    rgb_color_t trail_color = {
                        .r = (uint8_t)(explosion_collision_state.right_ion.color.r * trail_intensity),
                        .g = (uint8_t)(explosion_collision_state.right_ion.color.g * trail_intensity),
                        .b = (uint8_t)(explosion_collision_state.right_ion.color.b * trail_intensity)
                    };
                    
                    set_buffer_pixel_blend(trail_pos, trail_color, 0.5f);
                    
                    if (trail_intensity > 0.2f) {
                        rgb_color_t trail_glow = {
                            .r = (uint8_t)(trail_color.r * 0.4f),
                            .g = (uint8_t)(trail_color.g * 0.4f),
                            .b = (uint8_t)(trail_color.b * 0.4f)
                        };
                        
                        if (trail_pos - 1 >= 0) {
                            set_buffer_pixel_blend(trail_pos - 1, trail_glow, 0.3f);
                        }
                        if (trail_pos + 1 < led_count) {
                            set_buffer_pixel_blend(trail_pos + 1, trail_glow, 0.3f);
                        }
                    }
                }
            }
        }
    }
    
    // 绘制爆炸效果
    if (explosion_collision_state.explosion.active) {
        float intensity = explosion_collision_state.explosion.intensity;
        float radius = explosion_collision_state.explosion.radius;
        float pos = explosion_collision_state.explosion.position;
        
        int core_pos = (int)pos;
        if (core_pos >= 0 && core_pos < led_count) {
            float core_intensity = intensity * 2.0f; // 增加核心亮度
            if (core_intensity > 1.0f) core_intensity = 1.0f;
            
            rgb_color_t core_color = {
                .r = (uint8_t)(explosion_collision_state.explosion.core_color.r * core_intensity),
                .g = (uint8_t)(explosion_collision_state.explosion.core_color.g * core_intensity),
                .b = (uint8_t)(explosion_collision_state.explosion.core_color.b * core_intensity)
            };
            
            // 爆炸核心
            set_buffer_pixel(core_pos, core_color);
            
            // 强烈光晕
            for (int offset = 1; offset <= 4; offset++) {
                float glow_intensity = core_intensity * (0.7f / (offset * 0.6f));
                rgb_color_t glow_color = {
                    .r = (uint8_t)(core_color.r * glow_intensity),
                    .g = (uint8_t)(core_color.g * glow_intensity),
                    .b = (uint8_t)(core_color.b * glow_intensity)
                };
                
                if (core_pos - offset >= 0) {
                    set_buffer_pixel_blend(core_pos - offset, glow_color, 0.9f);
                }
                if (core_pos + offset < led_count) {
                    set_buffer_pixel_blend(core_pos + offset, glow_color, 0.9f);
                }
            }
        }
        
        // 冲击波
        if (radius > 0) {
            float shockwave_intensity = intensity * 0.8f;
            
            // 多层冲击波
            for (int ring = 0; ring < 3; ring++) {
                float ring_radius = radius - ring * 1.5f;
                if (ring_radius < 0) continue;
                
                float ring_intensity = shockwave_intensity * (0.9f / (ring + 1));
                
                for (int i = 0; i < led_count; i++) {
                    float distance = fabsf((float)i - pos);
                    float ring_width = 1.2f + ring * 0.3f;
                    
                    if (distance >= ring_radius - ring_width && distance <= ring_radius + ring_width) {
                        float ring_strength = 1.0f - fabsf(distance - ring_radius) / ring_width;
                        if (ring_strength < 0) ring_strength = 0;
                        
                        float final_intensity = ring_intensity * ring_strength;
                        
                        if (final_intensity > 0.02f) {
                            rgb_color_t shock_color;
                            
                            if (ring == 0) {
                                // 内层冲击波
                                shock_color = explosion_collision_state.explosion.shockwave_color;
                            } else if (ring == 1) {
                                // 中层冲击波
                                shock_color.r = (uint8_t)(220 * final_intensity);
                                shock_color.g = (uint8_t)(220 * final_intensity);
                                shock_color.b = (uint8_t)(240 * final_intensity);
                            } else {
                                // 外层冲击波
                                shock_color.r = (uint8_t)(180 * final_intensity);
                                shock_color.g = (uint8_t)(200 * final_intensity);
                                shock_color.b = (uint8_t)(220 * final_intensity);
                            }
                            
                            set_buffer_pixel_blend(i, shock_color, 1.0f);
                        }
                    }
                }
            }
        }
    }
    
    // 绘制爆炸粒子
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!explosion_collision_state.particles[i].active) continue;
        
        explosion_particle_t *p = &explosion_collision_state.particles[i];
        int particle_pos = (int)p->position;
        
        if (particle_pos >= 0 && particle_pos < led_count) {
            // 粒子颜色
            rgb_color_t particle_color = {
                .r = (uint8_t)(p->color.r * p->brightness),
                .g = (uint8_t)(p->color.g * p->brightness),
                .b = (uint8_t)(p->color.b * p->brightness)
            };
            
            // 绘制粒子核心
            set_buffer_pixel_blend(particle_pos, particle_color, 1.0f);
            
            // 粒子光晕
            for (int offset = 1; offset <= (int)p->size; offset++) {
                float glow_intensity = p->brightness * (0.3f / (offset * 0.8f));
                rgb_color_t glow_color = {
                    .r = (uint8_t)(particle_color.r * glow_intensity),
                    .g = (uint8_t)(particle_color.g * glow_intensity),
                    .b = (uint8_t)(particle_color.b * glow_intensity)
                };
                
                if (particle_pos - offset >= 0) {
                    set_buffer_pixel_blend(particle_pos - offset, glow_color, 0.4f);
                }
                if (particle_pos + offset < led_count) {
                    set_buffer_pixel_blend(particle_pos + offset, glow_color, 0.4f);
                }
            }
            
            // 高速运动时的拖尾
            if (fabsf(p->velocity) > 0.2f && p->direction != 0) {
                int trail_length = (int)(fabsf(p->velocity) * 2.0f);
                for (int t = 1; t <= trail_length; t++) {
                    int trail_pos = particle_pos - (t * p->direction);
                    if (trail_pos >= 0 && trail_pos < led_count) {
                        float trail_intensity = p->brightness * (0.3f / (t * 0.7f));
                        rgb_color_t trail_color = {
                            .r = (uint8_t)(particle_color.r * trail_intensity),
                            .g = (uint8_t)(particle_color.g * trail_intensity),
                            .b = (uint8_t)(particle_color.b * trail_intensity)
                        };
                        set_buffer_pixel_blend(trail_pos, trail_color, 0.3f);
                    }
                }
            }
        }
    }
    
    return apply_color_buffer();
}

#define PEAK_HOLD_MAX_LEDS 256
static float s_pk_level[PEAK_HOLD_MAX_LEDS];
static float s_pk_peak[PEAK_HOLD_MAX_LEDS];
static float s_pk_agc[PEAK_HOLD_MAX_LEDS];
static float s_sp_level[PEAK_HOLD_MAX_LEDS];
static float s_sp_agc[PEAK_HOLD_MAX_LEDS];

static float eq_hue_at(int i, int count)
{
    if (count <= 1) return 0.0f;
    return ((float)i / (float)(count - 1)) * 0.72f;
}

static float eq_norm(float raw, float *agc)
{
    float peak = *agc;
    if (raw > peak) {
        peak = raw;
    } else {
        peak = peak * 0.990f;
    }
    if (peak < 4.0f) peak = 4.0f;
    *agc = peak;
    float n = raw / peak;
    if (n > 1.0f) n = 1.0f;
    if (n < 0.0f) n = 0.0f;
    return powf(n, 0.6f);
}

static esp_err_t peak_hold_effect(float *energy_bands, int num_bands)
{
    if (!strip) return ESP_ERR_INVALID_STATE;
    if (led_count <= 0 || led_count > PEAK_HOLD_MAX_LEDS) return ESP_ERR_INVALID_ARG;

    int eff = num_bands - 1;
    if (eff <= 0) eff = 1;

    for (int i = 0; i < led_count; i++) {
        int band = (i * eff) / led_count + 1;
        if (band >= num_bands) band = num_bands - 1;

        float target = eq_norm(energy_bands[band], &s_pk_agc[i]);

        if (target > s_pk_level[i]) {
            s_pk_level[i] += (target - s_pk_level[i]) * 0.55f;
        } else {
            s_pk_level[i] += (target - s_pk_level[i]) * 0.05f;
        }

        if (s_pk_level[i] > s_pk_peak[i]) {
            s_pk_peak[i] = s_pk_level[i];
        } else {
            s_pk_peak[i] *= 0.984f;
        }
        if (s_pk_peak[i] < 0.002f) s_pk_peak[i] = 0.0f;

        float value = 0.10f + 0.90f * (0.45f * s_pk_level[i] + 0.55f * s_pk_peak[i]);
        value += fx_beat() * 0.5f;
        if (value > 1.0f) value = 1.0f;

        float hue = eq_hue_at(i, led_count);
        bool hot = ((s_pk_level[i] >= s_pk_peak[i] - 0.03f) && (s_pk_level[i] > 0.30f)) ||
                   (s_beat.onset && s_beat.bass > 25.0f);

        rgb_color_t color;
        if (hot) {
            uint8_t w = (uint8_t)(255.0f * s_pk_level[i]);
            color.r = w;
            color.g = w;
            color.b = w;
        } else {
            color = hsv_to_rgb(hue, 0.88f, value);
        }
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

static esp_err_t spectrum_effect(float *energy_bands, int num_bands)
{
    if (!strip) return ESP_ERR_INVALID_STATE;
    if (led_count <= 0 || led_count > PEAK_HOLD_MAX_LEDS) return ESP_ERR_INVALID_ARG;

    int eff = num_bands - 1;
    if (eff <= 0) eff = 1;

    for (int i = 0; i < led_count; i++) {
        int band = (i * eff) / led_count + 1;
        if (band >= num_bands) band = num_bands - 1;

        float n = eq_norm(energy_bands[band], &s_sp_agc[i]);
        float v = sqrtf(n);

        if (v > s_sp_level[i]) {
            s_sp_level[i] += (v - s_sp_level[i]) * 0.55f;
        } else {
            s_sp_level[i] += (v - s_sp_level[i]) * 0.12f;
        }
    }

    for (int i = 0; i < led_count; i++) {
        float glow = s_sp_level[i];
        if (i > 0) {
            float l = s_sp_level[i - 1] * 0.85f;
            if (l > glow) glow = l;
        }
        if (i + 1 < led_count) {
            float r = s_sp_level[i + 1] * 0.85f;
            if (r > glow) glow = r;
        }

        float value = 0.06f + 0.94f * glow;
        if (value > 1.0f) value = 1.0f;

        float hue = eq_hue_at(i, led_count);
        rgb_color_t color = hsv_to_rgb(hue, 0.92f, value);
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

static esp_err_t rainbow_effect(float *energy_bands, int num_bands)
{
    (void)energy_bands;
    (void)num_bands;
    for (int i = 0; i < led_count; i++) {
        float hue = (float)(i + animation_counter * g_fx.speed / 10.0) / led_count;
        hue = hue - (int)hue;
        rgb_color_t color = hsv_to_rgb(hue, 1.0, 0.5);
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

// 星空：多颗星辰在灯带上循环漂移，鼓点触发“超空间”加速+拖尾，高音让星点闪烁
#define SF_MAX 9
static struct { float pos; float spd; float size; float tw; float hue; } s_star[SF_MAX];
static bool s_star_init = false;

static esp_err_t starfield_effect(float *energy_bands, int num_bands)
{
    (void)energy_bands;
    (void)num_bands;
    if (!strip) return ESP_ERR_INVALID_STATE;
    if (led_count <= 0) return ESP_OK;

    if (!s_star_init) {
        for (int i = 0; i < SF_MAX; i++) {
            s_star[i].pos = ((float)rand() / RAND_MAX) * led_count;
            s_star[i].spd = 0.15f + (float)i / SF_MAX * 0.85f;
            s_star[i].size = 0.35f + ((float)rand() / RAND_MAX) * 0.65f;
            s_star[i].tw = ((float)rand() / RAND_MAX) * 6.283f;
            s_star[i].hue = 0.5f + ((float)rand() / RAND_MAX) * 0.15f;
        }
        s_star_init = true;
    }

    const led_beat_t *b = led_get_beat();
    float beat = fx_beat();
    float treble = fminf(b->high / 120.0f, 1.0f);

    clear_color_buffer();
    for (int i = 0; i < SF_MAX; i++) {
        float st = s_star[i].spd * (1.0f + beat * 3.5f) * fmaxf(g_fx.speed, 0.2f);
        s_star[i].pos += st;
        while (s_star[i].pos >= led_count) s_star[i].pos -= led_count;
        if (s_star[i].pos < 0) s_star[i].pos += led_count;

        float twinkle = 0.55f + 0.45f * sinf((float)animation_counter * (0.2f + treble * 0.9f) + s_star[i].tw);
        float bright = s_star[i].size * (0.6f + 0.7f * beat) * twinkle;
        if (bright > 1.0f) bright = 1.0f;
        float sigma = 0.7f + s_star[i].size * 0.8f;
        float sat = 0.85f - 0.6f * (0.5f * beat + 0.5f * treble);
        if (sat < 0.12f) sat = 0.12f;

        for (int led = 0; led < led_count; led++) {
            float dd = fabsf((float)led - s_star[i].pos);
            float wrap = (float)led_count - dd;
            float dist = dd < wrap ? dd : wrap;
            float g = expf(-(dist * dist) / (2.0f * sigma * sigma));
            float v = g * bright;
            if (v > 0.02f) {
                rgb_color_t color = hsv_to_rgb(s_star[i].hue, sat, v > 1.0f ? 1.0f : v);
                set_buffer_pixel_blend(led, color, v);
            }
        }
    }
    return apply_color_buffer();
}

static esp_err_t led_off_effect(float *energy_bands, int num_bands)
{
    (void)energy_bands;
    (void)num_bands;
    clear_color_buffer();
    return apply_color_buffer();
}

static esp_err_t mirror_effect(float *energy_bands, int num_bands)
{
    if (!strip) return ESP_ERR_INVALID_STATE;
    if (!energy_bands || num_bands <= 0) return ESP_OK;

    float maxe = 1.0f;
    for (int i = 0; i < num_bands; i++) if (energy_bands[i] > maxe) maxe = energy_bands[i];

    float half = (led_count - 1) * 0.5f;
    if (half < 1.0f) half = 1.0f;
    float beat = fx_beat();

    for (int i = 0; i < led_count; i++) {
        float d = fabsf((float)i - half) / half;   // 0=中心(低频) 1=两端(高频)
        int band = (int)(d * (num_bands - 1));
        if (band >= num_bands) band = num_bands - 1;

        float v = energy_bands[band] / maxe;
        v = sqrtf(v);
        v += 0.25f * beat * (1.0f - d);
        if (v > 1.0f) v = 1.0f;

        float value = 0.05f + 0.95f * v;
        float hue = d * 0.72f;                     // 左右对称：中心暖色 -> 边缘冷色
        rgb_color_t color = hsv_to_rgb(hue, 0.9f, value);
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

#define SHOCKWAVE_MAX 6
static struct { bool on; float wf; float strength; float hue; } s_sw[SHOCKWAVE_MAX];
static int s_sw_next = 0;

static esp_err_t shockwave_effect(float *energy_bands, int num_bands)
{
    (void)energy_bands;
    (void)num_bands;
    if (!strip) return ESP_ERR_INVALID_STATE;

    float half = (led_count - 1) * 0.5f;
    if (half < 1.0f) half = 1.0f;

    if (s_beat.onset) {
        int slot = -1;
        for (int k = 0; k < SHOCKWAVE_MAX; k++) if (!s_sw[k].on) { slot = k; break; }
        if (slot < 0) { slot = s_sw_next % SHOCKWAVE_MAX; s_sw_next++; }
        s_sw[slot].on = true;
        s_sw[slot].wf = 0.0f;
        s_sw[slot].strength = 1.0f;
        s_sw[slot].hue += 0.17f;
        if (s_sw[slot].hue > 1.0f) s_sw[slot].hue -= 1.0f;
    }

    for (int i = 0; i < led_count; i++) {
        float d = fabsf((float)i - half) / half;
        float acc = 0.0f, hue = 0.0f;
        for (int k = 0; k < SHOCKWAVE_MAX; k++) {
            if (!s_sw[k].on) continue;
            float dist = fabsf(d - s_sw[k].wf);
            float c = s_sw[k].strength * fmaxf(0.0f, 1.0f - dist / 0.18f);
            if (c > acc) { acc = c; hue = s_sw[k].hue; }
        }
        float core = fx_beat() * (1.0f - d);
        if (core > acc) { acc = core; hue = 0.0f; }
        if (acc > 1.0f) acc = 1.0f;

        float value = 0.03f + 0.97f * acc;
        rgb_color_t color = hsv_to_rgb(hue, 0.9f, value);
        led_out_pixel(i, color);
    }

    for (int k = 0; k < SHOCKWAVE_MAX; k++) {
        if (!s_sw[k].on) continue;
        s_sw[k].wf += 0.03f * g_fx.speed;
        s_sw[k].strength *= 0.955f;
        if (s_sw[k].strength < 0.04f || s_sw[k].wf > 1.15f) s_sw[k].on = false;
    }
    return ESP_OK;
}

static esp_err_t aurora_effect(float *energy_bands, int num_bands)
{
    if (!strip) return ESP_ERR_INVALID_STATE;
    float lvl = 0.35f;
    if (energy_bands && num_bands > 0) {
        float mx = 1.0f;
        for (int i = 0; i < num_bands; i++) if (energy_bands[i] > mx) mx = energy_bands[i];
        lvl = 0.25f + 0.75f * fminf(mx / 120.0f, 1.0f);
    }
    float t = (float)animation_counter * 0.05f * g_fx.speed;
    for (int i = 0; i < led_count; i++) {
        float x = (float)i / (led_count > 1 ? led_count - 1 : 1);
        float w1 = 0.5f + 0.5f * sinf(x * 6.2832f * 2.0f + t);
        float w2 = 0.5f + 0.5f * sinf(x * 6.2832f * 3.0f - t * 0.7f);
        float v = (w1 * 0.6f + w2 * 0.4f) * lvl;
        if (v > 1.0f) v = 1.0f;
        float value = 0.04f + 0.96f * v;
        float hue = x * 0.45f + t * 0.02f * g_fx.color_speed;
        rgb_color_t color = hsv_to_rgb(hue, 0.85f, value);
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

static esp_err_t heartbeat_effect(float *energy_bands, int num_bands)
{
    (void)energy_bands;
    (void)num_bands;
    if (!strip) return ESP_ERR_INVALID_STATE;

    static float hb_env = 0.0f;
    if (s_beat.onset) hb_env = 1.0f;
    hb_env *= 0.90f;
    float p = fmaxf(hb_env, fx_beat());

    float half = (led_count - 1) * 0.5f;
    if (half < 1.0f) half = 1.0f;
    for (int i = 0; i < led_count; i++) {
        float d = fabsf((float)i - half) / half;   // 中心最亮，向两端衰减
        float value = 0.04f + 0.96f * p * (1.0f - 0.6f * d);
        if (value > 1.0f) value = 1.0f;
        float hue = 0.97f;                          // 红色心跳
        rgb_color_t color = hsv_to_rgb(hue, 0.95f, value);
        led_out_pixel(i, color);
    }
    return ESP_OK;
}

typedef struct {
    led_mode_t mode;
    esp_err_t (*render)(float *energy_bands, int num_bands);
} led_effect_entry_t;

static const led_effect_entry_t s_led_effects[] = {
    { MODE_SPECTRUM, spectrum_effect },
    { MODE_RAINBOW, rainbow_effect },
    { MODE_STARFIELD, starfield_effect },
    { MODE_METEOR_PULSE, meteor_pulse_effect },
    { MODE_WATER_RIPPLE, water_ripple_effect },
    { MODE_ENERGY_WAVE, energy_wave_effect },
    { MODE_FIREWORKS, fireworks_effect_improved },
    { MODE_RHYTHM_PULSE, rhythm_pulse_effect },
    { MODE_RHYTHM_BREATH, rhythm_breath_effect },
    { MODE_RHYTHM_JUMP, rhythm_jump_effect },
    { MODE_SPARKLE_RAINBOW, sparkle_rainbow_effect },
    { MODE_EXPLOSION, explosion_collision_effect },
    { MODE_PEAK_HOLD, peak_hold_effect },
    { MODE_OFF, led_off_effect },
    { MODE_MIRROR, mirror_effect },
    { MODE_SHOCKWAVE, shockwave_effect },
    { MODE_AURORA, aurora_effect },
    { MODE_HEARTBEAT, heartbeat_effect },
};

#define LED_EFFECT_COUNT (sizeof(s_led_effects) / sizeof(s_led_effects[0]))

esp_err_t led_update_visualization(float *energy_bands, int num_bands)
{
    if (!strip) return ESP_ERR_INVALID_STATE;

    if (current_mode != MODE_RAINBOW && current_mode != MODE_STARFIELD &&
        current_mode != MODE_OFF && energy_bands == NULL) {
        static float zero_bands[NUM_FREQ_BANDS] = {0};
        energy_bands = zero_bands;
        num_bands = NUM_FREQ_BANDS;
    }

    if (energy_bands != NULL) {
        static float s_scaled[NUM_FREQ_BANDS];
        int nn = num_bands;
        if (nn > NUM_FREQ_BANDS) nn = NUM_FREQ_BANDS;
        for (int i = 0; i < nn; i++) s_scaled[i] = fx_sens(energy_bands[i]);
        energy_bands = s_scaled;
        num_bands = nn;
    }

    animation_counter++;

    led_beat_update(energy_bands, num_bands);
    led_update_display_spectrum(energy_bands, num_bands);

    for (size_t i = 0; i < LED_EFFECT_COUNT; i++) {
        if (s_led_effects[i].mode == current_mode) {
            esp_err_t ret = s_led_effects[i].render(energy_bands, num_bands);
            if (ret != ESP_OK) return ret;
            return led_strip_refresh(strip);
        }
    }

    led_clear_all();
    return led_strip_refresh(strip);
}
