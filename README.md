# ESP32-S3 双核音频可视化系统
<p align="center">
  <strong>基于ESP32-S3的实时音频可视化项目 | 双核协同处理 | Web无线控制 | 12种动态效果</strong>
</p>

<p align="center">
  <a href="#-项目简介">项目简介</a> •
  <a href="#-硬件需求">硬件需求</a> •
  <a href="#-软件架构">软件架构</a> •
  <a href="#-快速开始">快速开始</a> •
  <a href="#-使用方法">使用方法</a> •
  <a href="#-可视化效果库">可视化效果库</a> •
  <a href="#-贡献指南">贡献指南</a> •
  <a href="#-许可证">许可证</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--S3-ESP-IDF_5.1+-blue" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/双核处理-实时音频可视化-green" alt="双核处理">
  <img src="https://img.shields.io/badge/Web控制-无线配网-orange" alt="Web控制">
  <img src="https://img.shields.io/badge/可视化效果-12种模式-brightgreen" alt="12种模式">
</p>

## 📋 项目简介

这是一个基于ESP32-S3开发板的**实时音频可视化系统**，通过INMP441麦克风采集环境音频，进行FFT频域分析，并将分析结果实时映射到LED灯带上，创造出丰富的光影效果。项目采用双核架构设计，实现了高效的音频处理与LED渲染分离。

### ✨ 核心特性

- **🎵 高精度音频采集**：使用INMP441 MEMS麦克风，支持最高44100Hz采样率
- **⚡ 双核并行处理**：核心0处理WiFi通信与音频分析，核心1专责LED渲染
- **🌈 12种可视化模式**：包含频谱、流星脉冲、水波纹、能量波、烟花、节奏脉冲、节奏呼吸、节奏跳动、闪烁彩虹、爆炸碰撞等多种特效
- **🎨 智能颜色映射**：根据音频频率分布动态调整颜色，暖色对应低频，冷色对应高频
- **🌐 无线Web控制**：内置配网功能，可通过浏览器实时切换模式和调整参数
- **📊 实时系统监控**：通过串口或Web接口查看系统状态、帧率、内存使用等信息
- **🔊 多级音频响应**：低频、中频、高频分别触发不同效果，实现层次丰富的视觉反馈

## 🔧 硬件需求

### 必需组件

| 组件 | 规格 | 数量 | 备注 |
|------|------|------|------|
| ESP32-S3开发板 | 带WiFi，双核240MHz | 1 | 推荐ESP32-S3-DevKitC-1 |
| INMP441麦克风模块 | I2S接口，24位ADC | 1 | 或类似I2S麦克风 |
| WS2812B LED灯带 | 5V，任意长度 | 1 | GPIO 5控制，支持多种效果 |
| 电源 | 5V/3A以上 | 1 | 根据LED数量选择 |

### 可选组件

- **电平转换模块**：如果ESP32与LED灯带电压不匹配
- **音频输入模块**：用于线路输入（需代码适配）
- **外壳**：3D打印或定制外壳
- **散热装置**：长时间高亮度运行建议增加散热

### 硬件连接

| ESP32-S3引脚 | 连接目标 | 备注 |
|-------------|----------|------|
| GPIO 5 | LED灯带DATA | 数据线（效果控制引脚） |
| GPIO 42 | INMP441 BCK | 位时钟 |
| GPIO 41 | INMP441 WS | 字选择 |
| GPIO 40 | INMP441 DATA | 数据输出 |
| 5V | LED灯带+5V | 电源正极 |
| GND | LED灯带GND | 电源负极 |
| 3.3V | INMP441 VDD | 麦克风电源 |
| GND | INMP441 GND | 麦克风地线 |

> **注意**：如果LED数量较多（>50颗），请使用外部5V电源，避免ESP32的3.3V转5V模块过载。对于12种可视化效果，需要足够的电源功率以保证效果稳定性。

## 🏗️ 软件架构

### 系统架构图

```
┌─────────────────────────────────────────────────────────┐
│                    ESP32-S3 双核系统                     │
├───────────────┬─────────────────────────────────────────┤
│   核心0       │           核心1                          │
│ (WiFi/音频)   │        (LED渲染)                         │
├───────────────┼─────────────────────────────────────────┤
│ • WiFi管理    │ • LED控制器驱动                          │
│ • Web服务器   │ • 12种可视化算法                         │
│ • FFT音频分析 │ • 实时渲染引擎                           │
│ • 双核通信    │ • 颜色缓冲区管理                         │
│ • 系统监控    │ • 物理模拟引擎                           │
│ • 音频预处理  │ • 粒子系统管理                           │
└───────────────┴─────────────────────────────────────────┘
         │                            │
         ▼                            ▼
┌──────────────┐             ┌─────────────────┐
│ INMP441麦克风 │             │ WS2812B LED灯带 │
│   (I2S)      │             │   (PWM)         │
└──────────────┘             └─────────────────┘
```

### 主要模块说明

#### 1. 音频处理流水线 (`audio_processor.c`)
- **音频采集**：通过I2S接口从INMP441读取24位音频数据
- **预处理**：高通滤波去除直流偏移，应用汉宁窗减少频谱泄漏
- **FFT分析**：使用ESP-DSP库进行快速傅里叶变换（32位浮点）
- **频带划分**：将频谱划分为32个对数频带，精确适配人耳听觉特性
- **能量平滑**：指数平滑处理，避免频闪现象

#### 2. LED渲染引擎 (`led_core.c`, `led_controller.c`)
- **模式管理**：12种预设可视化模式
- **实时渲染**：根据音频数据动态生成颜色和动画，最高支持100FPS
- **颜色缓冲区**：使用混合渲染系统，支持多层效果叠加
- **物理模拟**：包含弹簧阻尼系统、粒子系统、流体动力学模拟
- **智能触发**：基于音频能量动态触发特效，避免过度重复

#### 3. 可视化效果库
- **基础效果**：频谱分析、彩虹流动、调试模式
- **动态特效**：流星脉冲、水波纹、能量波、烟花绽放
- **节奏响应**：节奏脉冲、节奏呼吸、节奏跳动
- **高级动画**：闪烁彩虹、爆炸碰撞

#### 4. 双核通信机制 (`dual_core_com.c`)
- **音频数据队列**：核心0→核心1传输频带能量数据（32频带）
- **命令队列**：核心0←→核心1传输控制命令（模式切换、亮度调节等）
- **状态同步**：实时同步WiFi连接状态、帧率统计、内存使用等信息
- **零拷贝传输**：使用共享内存提高数据传输效率

#### 5. 网络控制接口 (`wifi_core.c`)
- **智能配网**：首次启动进入AP模式，通过Web界面配置WiFi
- **Web服务器**：提供RESTful API控制接口，支持实时模式切换
- **状态监控**：实时查看系统状态和性能指标，JSON格式返回
- **OTA支持**：支持通过Web界面进行固件升级

## 🚀 快速开始

### 1. 环境准备

#### 安装ESP-IDF
```bash
# 克隆ESP-IDF（推荐使用v5.1+）
mkdir -p ~/esp
cd ~/esp
git clone -b v5.1.3 --recursive https://github.com/espressif/esp-idf.git

# 设置工具链（仅首次运行）
cd esp-idf
./install.sh esp32s3

# 激活环境（每次新终端会话）
source export.sh
```


#### 安装依赖组件

```bash
# ESP-IDF v5.x 会自动从 component server 下载依赖（esp-dsp, led_strip 等）
# 首次编译时会提示下载组件，按提示操作即可
```

### 2. 硬件连接
按照上述硬件连接表正确连接所有组件，特别注意：
- **电源隔离**：LED灯带使用独立5V电源，避免干扰音频采集
- **信号线长度**：LED数据线尽量短于50cm，避免信号衰减
- **接地处理**：确保所有GND连接到同一参考点

### 3. 配置项目
```bash
# 进入项目目录
cd ~/esp/esp32-FFTVISIUAL1.0WS2812B

# 设置目标芯片
idf.py set-target esp32s3

# 打开配置界面
idf.py menuconfig
```

关键配置项：
- **Partition Table**: 选择默认的单分区表或自定义分区表
- **PSRAM**: 如果板载PSRAM，启用`SPIRAM`支持（提升性能）
- **WiFi SSID/Password**: 可在代码中配置或使用配网功能
- **Audio Settings**: 音频采样率、FFT大小、增益等参数
- **LED Settings**: LED数量、GPIO引脚、亮度限制
- **Effect Parameters**: 各可视化效果的参数调整

### 4. 编译与烧录
```bash
# 编译项目（首次编译需要较长时间）
idf.py build

# 烧录到ESP32（替换为你的串口）
idf.py -p /dev/ttyUSB0 flash

# 监控串口输出（查看启动日志）
idf.py -p /dev/ttyUSB0 monitor
```
> 按 `Ctrl+]` 退出串口监视器

### 5. 固件调试
如果遇到问题，可以：
```bash
# 擦除闪存重新烧录
idf.py -p /dev/ttyUSB0 erase_flash

# 查看详细构建信息
idf.py size-components
idf.py size-files

# 生成内存使用报告
idf.py size
```

## 📖 使用方法

### 首次启动与配网

1. **上电启动**：系统启动后，如果没有保存的WiFi配置，会自动进入配网模式
2. **连接热点**：用手机或电脑连接名为 `ESP32-S3-Visualizer` 的热点（密码：`12345678`）
3. **访问配网页面**：浏览器访问 `192.168.175.126`
4. **配置WiFi**：在页面中选择你的WiFi网络并输入密码
5. **自动重启**：配置成功后，设备会自动重启并连接到指定WiFi

### 获取设备IP地址

配网成功后，查看串口监视器，会显示分配到的IP地址：
```
I (1234) WIFI_CORE: WiFi连接成功！
I (1235) WIFI_CORE: 获取到IP: 192.168.175.126
I (1236) LED_CTRL: LED控制器初始化成功
I (1237) AUDIO_PROC: 音频处理器就绪，采样率: 44100Hz
```

### Web控制界面

访问 `http://[设备IP]` 进入Web控制界面，支持以下功能：

#### API端点
| 端点 | 方法 | 功能 | 参数 | 示例 |
|------|------|------|------|------|
| `/api/status` | GET | 获取系统状态 | 无 | `curl http://192.168.175.126/api/status` |
| `/api/mode` | GET | 获取当前模式 | 无 | `curl http://192.168.175.126/api/mode` |
| `/api/mode` | POST | 设置模式 | `mode=0-12` | `curl -X POST "http://192.168.175.126/api/mode?mode=4"` |
| `/api/effects` | GET | 获取效果列表 | 无 | `curl http://192.168.175.126/api/effects` |
| `/api/brightness` | POST | 设置亮度 | `value=0-255` | `curl -X POST "http://192.168.175.126/api/brightness?value=150"` |
| `/api/audio` | GET | 获取音频状态 | 无 | `curl http://192.168.175.126/api/audio` |
| `/ping` | GET | 连通性测试 | 无 | `curl http://192.168.175.126/ping` |

### 串口命令控制

通过串口监视器发送命令：
```
# 查看当前状态
status

# 切换模式（0-12）
mode 4

# 设置亮度（0-255）
brightness 200

# 测试LED
test

# 查看帧率
fps

# 查看内存使用
memory
```

## 🌈 可视化效果库

本系统包含12种精心设计的可视化效果，每种效果都有独特的音频响应特性：

### 1. 频谱分析 (Mode 0)
**描述**: 经典频谱柱状图，按频率分布显示，低音红色，高音蓝色
**音频响应**: 直接映射32个频带能量到LED高度
**适用场景**: 通用音乐可视化，清晰显示频率分布
**参数**: 动态范围、颜色映射、平滑系数

### 2. 彩虹光谱 (Mode 1)
**描述**: 流动的彩虹效果，色彩随时间平滑过渡
**音频响应**: 整体亮度随音频能量变化
**适用场景**: 背景氛围灯，柔和视觉效果
**参数**: 流动速度、饱和度、亮度范围

### 3. 调试模式 (Mode 2)
**描述**: 红绿蓝三色测试，用于硬件调试和LED检测
**音频响应**: 无音频响应
**适用场景**: 硬件测试、LED故障排查
**参数**: 测试图案、颜色强度

### 4. 流星脉冲 (Mode 4)
**描述**: 流星划过效果，带有拖尾和光晕，速度随节奏变化
**音频响应**: 低频触发流星生成，速度随能量变化
**适用场景**: 动态感强的音乐，特别是电子音乐
**参数**: 流星数量、速度范围、拖尾长度、光晕强度

### 5. 水波纹 (Mode 5)
**描述**: 音频触发的水面涟漪效果，多颜色动态波纹
**音频响应**: 低频产生大波纹，高频产生小波纹
**适用场景**: 环境音乐、自然声效
**参数**: 波纹类型、传播速度、衰减率、颜色变化

### 6. 能量波 (Mode 6)
**描述**: 动态频谱均衡器效果，能量波从中心向外扩散
**音频响应**: 各频段独立响应，节拍增强效果
**适用场景**: 均衡器显示，节奏强烈的音乐
**参数**: 频段数量、峰值保持、节拍灵敏度

### 7. 烟花绽放 (Mode 7)
**描述**: 音频峰值触发彩色烟花爆炸，带有粒子效果
**音频响应**: 低频触发主烟花，高频触发小火花
**适用场景**: 音乐高潮部分，庆祝氛围
**参数**: 烟花强度、爆炸半径、粒子数量、颜色类型

### 8. 节奏脉冲 (Mode 8)
**描述**: LED随节奏同步脉冲，随机方向发射光波
**音频响应**: 低中高频分别触发不同方向脉冲
**适用场景**: 节奏感强的音乐，舞曲
**参数**: 脉冲速度、长度、强度、颜色映射

### 9. 节奏呼吸 (Mode 9)
**描述**: 柔和呼吸效果，速度随音乐变化，带节拍闪烁
**音频响应**: 低频节拍触发中心闪烁
**适用场景**: 放松音乐，氛围照明
**参数**: 呼吸速度、深度、节拍阈值、闪烁颜色

### 10. 节奏跳动 (Mode 10)
**描述**: 弹簧物理模拟的跳动效果，各频段独立响应
**音频响应**: 分8个频段独立控制高度，质量-弹簧-阻尼系统
**适用场景**: 复杂音乐，显示多层次节奏
**参数**: 刚度、阻尼、重力、响应速度、亮度增强

### 11. 闪烁彩虹 (Mode 11)
**描述**: 动态彩虹背景上发射彩色闪烁颗粒
**音频响应**: 能量变化触发颗粒发射，能量决定颗粒速度
**适用场景**: 活泼欢快的音乐，派对氛围
**参数**: 彩虹速度、颗粒大小、发射间隔、能量阈值

### 12. 爆炸碰撞 (Mode 12)
**描述**: 离子对撞产生彩色爆炸和粒子附着效果
**音频响应**: 高能量触发离子发射，碰撞产生爆炸
**适用场景**: 音乐高潮，戏剧性效果
**参数**: 离子速度、碰撞区域、爆炸强度、粒子寿命

## 🔧 效果参数调优

每种效果都支持参数调整，可以通过修改`led_controller.c`中的默认参数或通过API动态调整：

### 通用参数
```c
// 亮度控制
#define MAX_BRIGHTNESS 200    // 最大亮度（0-255）
#define MIN_BRIGHTNESS 30     // 最小亮度

// 响应速度
#define ENERGY_SMOOTHING 0.7f // 能量平滑系数（0-1）
#define RESPONSE_SPEED 0.5f   // 响应速度

// 颜色映射
#define COLOR_SATURATION 0.9f // 颜色饱和度
#define COLOR_SPEED 0.003f    // 颜色变化速度
```

### 效果特定参数
```c
// 流星脉冲
#define MAX_METEORS 4         // 同时存在的最大流星数
#define METEOR_MIN_SPEED 0.2f // 最小速度
#define METEOR_MAX_SPEED 0.8f // 最大速度

// 节奏跳动
#define JUMP_SEGMENTS 8       // 跳动段数
#define JUMP_STIFFNESS 0.3f   // 弹簧刚度
#define JUMP_DAMPING 0.85f    // 阻尼系数

// 爆炸碰撞
#define MAX_PARTICLES 60      // 最大粒子数
#define COLLISION_ZONE 0.5f   // 碰撞区域中心
```

## 🛠️ 故障排除

### 常见问题及解决方案

1. **麦克风无信号或噪音大**
   ```
   症状：LED效果不随音乐变化或响应异常
   检查：
   - INMP441接线是否正确（BCK, WS, DATA, VDD, GND）
   - 麦克风VDD是否接3.3V（不是5V）
   - 麦克风是否面向声源
   - 环境噪音是否过大
   解决：
   - 调整audio_processor.c中的增益参数
   - 添加软件降噪滤波器
   - 检查采样率设置（建议44100Hz）
   ```

2. **LED灯带闪烁、颜色异常或不亮**
   ```
   症状：LED显示不稳定、颜色错误或完全不亮
   检查：
   - 电源是否充足（5V/3A以上）
   - 数据线方向是否正确（箭头指向远离ESP32）
   - GPIO引脚配置是否匹配（默认GPIO 5）
   - LED数量配置是否正确（led_controller_init参数）
   解决：
   - 增加电源功率或减少LED数量
   - 在数据线靠近ESP32端添加100Ω电阻
   - 检查并修复接地问题
   - 在menuconfig中调整RMT分辨率
   ```

3. **系统卡顿、掉帧或重启**
   ```
   症状：效果不流畅、卡顿或设备频繁重启
   检查：
   - 串口日志是否有内存不足错误
   - 双核通信队列是否溢出
   - FFT计算是否超时
   - 电源是否稳定
   解决：
   - 减少LED数量或降低亮度
   - 优化FFT大小（256或512点）
   - 增加双核队列大小
   - 启用PSRAM（如有）
   - 检查并修复内存泄漏
   ```

4. **WiFi连接不稳定或配网失败**
   ```
   症状：无法连接WiFi或频繁断开
   检查：
   - WiFi密码是否正确
   - 路由器是否支持2.4GHz频段
   - 信号强度是否足够
   - 是否有IP地址冲突
   解决：
   - 重启路由器
   - 调整AP信道（避开拥挤信道）
   - 增加WiFi重连次数
   - 使用静态IP避免冲突
   ```

5. **可视化效果不理想**
   ```
   症状：效果不明显或响应不准确
   检查：
   - 音频预处理参数是否合适
   - 能量阈值设置是否合理
   - 效果触发逻辑是否正确
   - 颜色映射是否符合预期
   解决：
   - 调整能量归一化参数
   - 修改触发阈值和灵敏度
   - 优化颜色映射算法
   - 增加效果变化多样性
   ```

### 调试工具和技巧

1. **串口调试信息**
   ```bash
   # 启用详细调试日志
   idf.py menuconfig
   # Component config → Log output → Default log verbosity → Debug
   
   # 查看实时帧率
   I (45678) LED_CTRL: 当前帧率: 98.5 FPS
   
   # 查看内存使用
   I (45679) SYSTEM: 空闲堆内存: 125632 bytes
   I (45680) SYSTEM: 最小空闲堆: 89234 bytes
   ```

2. **性能监控**
   ```bash
   # 通过Web接口获取性能数据
   curl http://1192.168.175.126/api/status
   
   # 响应示例
   {
     "status": "running",
     "mode": 4,
     "fps": 98.5,
     "free_heap": 125632,
     "audio_energy": 45.6,
     "wifi_rssi": -65,
     "uptime": 3600
   }
   ```

3. **音频调试工具**
   ```c
   // 在audio_processor.c中启用音频调试
   #define AUDIO_DEBUG 1
   
   // 输出音频能量分布
   ESP_LOGI(TAG, "能量分布: 低频=%.1f, 中频=%.1f, 高频=%.1f", 
           low_energy, mid_energy, high_energy);
   ```

4. **LED测试模式**
   ```bash
   # 通过串口发送测试命令
   # 测试所有LED（红绿蓝白）
   test all
   
   # 测试单个LED
   test pixel 10 255 0 0
   
   # 测试彩虹效果
   test rainbow
   ```

### 高级调试

1. **内存泄漏检测**
   ```bash
   # 启用堆内存调试
   idf.py menuconfig
   # Component config → Heap Memory Debugging → Enable heap tracing
   
   # 在代码中添加堆检查点
   heap_caps_check_integrity_all(true);
   ```

2. **性能分析**
   ```c
   // 使用ESP32内置性能计数器
   #include "esp_timer.h"
   
   uint64_t start_time = esp_timer_get_time();
   // 执行代码
   uint64_t end_time = esp_timer_get_time();
   ESP_LOGI(TAG, "执行时间: %lld us", end_time - start_time);
   ```

3. **双核通信监控**
   ```c
   // 检查队列状态
   UBaseType_t uxMessagesWaiting = uxQueueMessagesWaiting(audio_data_queue);
   ESP_LOGI(TAG, "音频队列等待消息数: %d", uxMessagesWaiting);
   ```

## 🤝 贡献指南

我们欢迎任何形式的贡献！以下是参与本项目的方式：

### 报告问题
- 使用 [GitHub Issues](https://github.com/your-username/FFTVISIUAL1.0WS2812B/issues) 报告 bug 或提出功能建议
- 提供详细的复现步骤、环境信息和日志输出
- 附上相关代码片段或配置文件

### 开发规范
- 遵循ESP-IDF代码风格指南
- 新增功能需要包含相应的文档更新
- 确保代码通过编译且不影响现有功能
- 添加适当的错误处理和日志输出
- 进行基本的性能测试

### 添加新效果
如果你想添加新的可视化效果，请遵循以下步骤：

1. **在`led_controller.h`中添加模式枚举**
   ```c
   typedef enum {
     // ... 现有模式
     MODE_YOUR_NEW_EFFECT,  // 你的新效果
     MODE_COUNT
   } led_mode_t;
   ```

2. **实现效果函数**
   ```c
   static esp_err_t your_new_effect(float *energy_bands, int num_bands) {
     // 效果实现
   }
   ```

3. **在`led_update_visualization`中添加模式分支**
   ```c
   case MODE_YOUR_NEW_EFFECT:
     return your_new_effect(energy_bands, num_bands);
   ```

4. **添加效果参数配置**
   ```c
   // 在文件顶部添加效果专用变量
   static struct {
     // 效果状态变量
   } your_effect_state;
   ```

5. **更新文档**
   - 更新README中的效果列表
   - 添加效果说明和使用示例

## 📁 项目结构

```
/FFTVISIUAL1.0WS2812B
├── main/
│   ├── main.c                 # 主程序入口，系统初始化
│   └── CMakeLists.txt
├── components/
│   ├── audio_processor/      # FFT 音频处理模块
│   │   ├── audio_processor.c
│   │   ├── audio_processor.h
│   │   ├── CMakeLists.txt
│   │   └── include/
│   ├── dual_core_com/        # 双核通信模块
│   │   ├── dual_core_com.c
│   │   ├── dual_core_com.h
│   │   └── CMakeLists.txt
│   ├── led_core/             # LED 渲染核心
│   │   ├── led_core.c
│   │   ├── led_core.h
│   │   ├── CMakeLists.txt
│   │   └── include/
│   ├── led_controller/       # LED 控制器（12 种效果实现）
│   │   ├── led_controller.c
│   │   ├── led_controller.h
│   │   ├── CMakeLists.txt
│   │   └── include/
│   ├── wifi_core/            # WiFi 和 Web 服务
│   │   ├── wifi_core.c
│   │   ├── wifi_core.h
│   │   └── CMakeLists.txt
│   ├── inmp441_mic/          # 麦克风驱动
│   │   ├── inmp441_mic.c
│   │   ├── inmp441_mic.h
│   │   └── CMakeLists.txt
│   └── utils/                # 工具函数
│       ├── utils.c
│       └── CMakeLists.txt
├── led-controller.html       # Web 控制界面（示例）
├── CMakeLists.txt            # 项目 CMake 配置
├── idf_component.yml         # ESP-IDF 组件依赖配置
├── sdkconfig.defaults        # SDK 默认配置
└── README.md                 # 本文档
```

## 📄 许可证

本项目采用 **MIT许可证** - 查看 [LICENSE](LICENSE) 文件了解详情。

## 🙏 致谢

- **Espressif Systems** - 提供优秀的ESP32平台和ESP-IDF框架
- **ESP-DSP库开发者** - 提供高效的FFT实现
- **所有贡献者** - 感谢每一位为本项目做出贡献的开发者
- **开源社区** - 感谢所有开源项目的支持和灵感

## 📞 联系方式

如有问题或建议，欢迎通过以下方式交流：

- **GitHub Issues**: [提交 Issue](https://github.com/oyc521/FFTvisiual1.0ws2812b/issues)
- **Discussions**: [项目讨论区](https://github.com/oyc521/FFTvisiual1.0ws2812b/discussions)



---

<p align="center">
  <sub>用代码创造光影，用音乐点亮生活 ✨</sub>
</p>

<p align="center">
  <sub>如果你喜欢这个项目，请给它一个 ⭐️ 支持！</sub>
</p>

---

## 🎵 WiFi 音频推流（可选：麦克风 / WiFi 双音源）

设备支持两种音源，可随时热切换，**默认麦克风，零回归**：

- **麦克风**：INMP441 经 I2S 采集环境音（原始方案）。
- **WiFi 推流**：把**电脑/手机正在播放的音乐**通过局域网 UDP 推给设备，无需外接麦克风。切换：控制台“音频源”按钮，或 `POST /api/source?src=wifi|mic`。

> 数据链路：`WASAPI 回环(电脑) → 单声道 int16 44.1k → UDP:5004 → 设备环形缓冲 → FFT → 灯效/实时频谱`。FFT/灯效/后处理完全复用，只是“音频入口”换了来源。

### 方案 A：电脑双击启动（推荐日常）
1. 装一次依赖：`python -m pip install pyaudiowpatch numpy`
2. 双击 `start_audio_push.bat` —— 自动发现设备（或用上次的 IP 缓存 `~/.esp_led_ip.txt`）、自动打开控制台、开始把“电脑正在播放的声音”推给设备；关窗口即停。
3. 需要停止后台残留：双击 `stop_audio_push.bat`。

### 方案 C：网页下载启动器（给没装东西的电脑）
- 控制台点 **“⬇ 下载电脑启动器”** → 得到 `start_esp_audio.bat`（已自动填入设备 IP）。双击它：自动 `pip` 装依赖 → 从设备拉 `loopback.py` → 开始推流。

### 端口 / 端点 / 排障
- 音频 UDP **5004**；设备自动发现 UDP **5005**（发 `ESPLED` → 回 `ESPLED <ip> <port>`）。
- 相关接口：`POST /api/source?src=…`、`GET /api/source`、`GET /api/status`（含 `source/wifi_streaming`）、`GET /start.bat`、`GET /loopback.py`。
- 控制台已**内嵌进设备**：任意浏览器直接开 `http://<设备IP>/` 即可（同源，无需本地文件）。
- 前提：电脑与设备**同一网络**、Python 在 PATH；首次发现若弹防火墙请点“允许”（之后走缓存）。发现不到可手动：`python tools\wifi_audio_loopback.py <设备IP>`。
- 说明：ESP32-S3 无经典蓝牙，**A2DP 蓝牙音乐不可行**，故音源走 Wi‑Fi 推流。

---

**更新日志**:
- **2026-09-24**: 新增 **OTA 无线升级（A/B 双分区 + 回滚）**、**统一效果参数**（speed/intensity/sensitivity/hue/color_speed/beat_react）、**节拍/音频特征引擎**、**成像后处理**（软件调光/Gamma/噪声门/余晖）、**Web 实时 32 柱频谱**，并新增 **弹跳小球/波峰余晖/中心对称频谱/节拍冲击波/极光辉光/心跳/星空** 等模式；加入 **WiFi 音频推流（麦克风·WiFi 双音源）** 与一键启动器。
- **2025-12-29**: 新增12种可视化效果，包括流星脉冲、水波纹、能量波、烟花绽放、节奏脉冲、节奏呼吸、节奏跳动、闪烁彩虹、爆炸碰撞,双核通信机制，提升帧率稳定性,添加Web控制接口，支持实时模式切换

**未来计划**:
- 添加更多可视化效果
- 支持蓝牙音频输入
- 添加移动端APP控制
- 支持多设备同步
- 添加声音识别功能（掌声、节拍检测等）
- 蓝牙频谱音响
