# OTA 学习笔记(基于本工程 ESP-IDF v6.1 / ESP32-S3)

> 配套代码:分区表 `partitions.csv`、组件 `components/ota_updater/`、
> 集成点 `wifi_core.c`(/api/ota、CORS、版本上报)、`main.c`(启动确认)、
> 网页 `led-controller.html`(升级按钮 + 进度条)。

---

## 一、OTA 完整链路

### 1.1 一张图讲清

```
① 编译出 .bin(版本号写在镜像头 app_desc)
        │
② 分区表: nvs | otadata | factory | ota_0 | ota_1
        │
③ 把 .bin 送到另一个空闲槽
   选槽 → esp_ota_begin → 边收边写 esp_ota_write → esp_ota_end → esp_ota_set_boot_partition
        │  此刻只是“登记意图”,不生效
④ esp_restart()
        │
⑤ bootloader 读 otadata → 决定启动 factory / ota_0 / ota_1 → 校验镜像
        │
⑥ 新固件跑起来,自检 OK → esp_ota_mark_app_valid_cancel_rollback() 转正
        │
⑦ 版本可见:/api/status 返回 fw_version → 确认升级成功
```

### 1.2 每个环节的“为什么”

- **两个槽而不是覆盖自己**:运行中的固件代码在 flash 上执行,不能自我覆盖。双槽 = 写一个、跑一个,失败随时切回 = A/B 方案。
- **otadata 只要 8KB**:不装固件,只存“该启动谁 + 状态(valid/pending/aborted)”。8KB = 两个 4KB 扇区交替写 + CRC,抗掉电。
- **esp_ota_set_boot_partition 不是立即切换**:只写 otadata;真正切换在**重启后由 bootloader** 完成,所以 OTA 期间设备能继续干活。
- **流式 esp_ota_write**:网络一帧写一帧,内存只需一个缓冲(本工程 4KB),与固件大小无关。
- **esp_ota_end 校验 + 掉电兜底**:长度/镜像头错误会返回失败、不设启动;即使中途断电,坏镜像过不了 bootloader 校验,自动回退上一版 → 不易变砖。
- **回滚 = 双槽真正的价值**:新版本自检没过/未确认 → 复位后自动回退。
- **版本号**:没有它无法回答“设备跑的是哪个固件、升级是否成功”。

### 1.3 push vs pull(本地升级 vs 量产)

```
本地/学习: 设备开 HTTP 服务,人用网页/curl POST 上传 .bin   ← 本工程现状
商用/量产: 设备主动 GET https://服务器/version.json
           → 版本不同 → esp_https_ota(URL) 下载写下一槽
           → 需要 HTTPS + 固件签名 + 灰度/分批
```

---

## 二、本工程实现对照表

| 知识点 | 代码位置 | 说明 |
|---|---|---|
| 分区表 | `partitions.csv` | nvs(0x9000 不变,保住配网) + otadata + factory/ota_0/ota_1(各 0x180000) |
| 配置 | `sdkconfig.defaults` | custom 分区、flash 16MB、`BOOTLOADER_APP_ROLLBACK_ENABLE=y` |
| 上传写入 | `ota_updater.c` | POST /api/ota 流式接收,begin/write/end/set_boot/延时重启 |
| 错误处理 | `ota_updater.c` | 并发单飞、超槽 413、中断 abort、镜像无效 400、繁忙 503 |
| 回滚确认 | `ota_updater.c` + `main.c` | `esp_ota_mark_app_valid_cancel_rollback()`,放在系统自检完成后 |
| 版本上报 | `wifi_core.c` | `esp_app_get_description()` → fw_version |
| CORS | `wifi_core.c` / `ota_updater.c` | 让 file:// 本地网页能跨域访问 |
| 网页 UI | `led-controller.html` | 选文件 + XHR 进度条 + 重启后自动轮询恢复 |

**接口速查**:`POST /api/ota`(body=原始 .bin);`GET /api/status`(含 fw_version);`GET /ping`。

---

## 三、概念细节

- **esp_ota_get_next_update_partition(running)**:返回“非当前正在跑”的空槽。
- **OTA_SIZE_UNKNOWN**:让 esp_ota_write 写满为止,由 esp_ota_end 核对实际字节数;也可先做上限保护。
- **esp_ota_abort** vs **esp_ota_end**:中途失败用 abort 放弃;正常收尾用 end(会校验)。
- **回滚流程**:set_boot_partition(带 rollback)→ otadata=PENDING_VERIFY → bootloader 启动 → 应用自检通过后 mark valid → 状态 VALID;未确认就复位 → 自动回退。
- **版本**:`project(... VERSION)` 写入 app descriptor,运行时 `esp_app_get_description()->version`;bootloader 也能读。
- **防降级**:Anti-Rollback(`secure_version`),通常配 Secure Boot V2;只升不降,慎用。
- **安全量产**:HTTPS(esp_https_ota)+ 固件签名 + Secure Boot + 灰度分批。

---

## 四、面试高频问题与标准答案

**Q1 什么是 OTA?为什么用双分区?**
A/B(两个 app 槽 + otadata):运行中固件不能自我覆盖;双槽能“写新跑旧”,启动失败自动回退,降低变砖风险。

**Q2 OTA 分区表怎么设计?**
nvs(配置)+ otadata(启动选择与状态)+ 两个 app(ota_0/ota_1,可留 factory 兜底),app 需 0x10000 对齐;nvs 偏移生产要固定,否则丢用户配置。

**Q3 begin/write/end/set_boot_partition 职责?**
get_next_update_partition 选槽 → begin 开写会话(OTA_SIZE_UNKNOWN 写满为止)→ write 分块流式写 → end 收尾校验 → set_boot_partition 写 otadata(重启生效)→ esp_restart。

**Q4 升级掉电会砖吗?**
基本不会:坏镜像过不了 bootloader 校验 → 回退旧版;otadata 双扇区交替 + CRC 抗掉电。

**Q5 回滚原理?mark valid 时机?**
开 rollback 后新镜像标记 pending;启动后自检通过再 `esp_ota_mark_app_valid_cancel_rollback` 转正;未确认就复位则自动回退。确认要放在“确认系统健康”之后,不能一上电就做。

**Q6 版本从哪来怎么读?**
CMake VERSION / git tag → 写进 app descriptor;运行时 esp_app_get_description();/api/status 暴露,用于确认升级结果。

**Q7 怎么防刷回旧版本?**
Anti-Rollback(secure_version)+ Secure Boot V2;每次发版递增,只升不降,需配灰度。

**Q8 云端 OTA 为什么不裸 HTTP?**
esp_https_ota + 合法证书/根证书;裸 HTTP 可被中间人替换固件。量产再加固件签名。

**Q9 如何测试 OTA?**
正常升/降版本、故意不 confirm 验证回退、升级中随机断电、网络中断/Content-Length 异常、边界(等于/超过槽大小)、重复触发并发。

**Q10 1000 台设备 OTA 架构?**
设备:开机/定时查 version → 后台下载到空槽 → 自检 → 确认 → 上报。服务端:仓库 + version.json + 灰度分组/停发;CDN/OSS 扛带宽;HTTPS+签名;监控成功率并支持定向回滚。

---

## 五、面试话术模板

> 我基于 ESP-IDF v6.1 / ESP32-S3 给音频可视化项目实现了 OTA:
> 采用 A/B 双分区 + otadata,用 esp_ota_begin→write→end→set_boot_partition 流式写入空闲槽,成功后重启切换;
> 开启 rollback,在系统自检完成后调用 esp_ota_mark_app_valid_cancel_rollback 转正,失败可自动回退;
> 同时实现版本上报(/api/status 的 fw_version)、升级接口 POST /api/ota(并发保护/超限/镜像校验)、网页升级 UI(进度条,并处理设备重启导致断连的边界)。
> 真机走完 1.0.0→1.0.1→1.0.2 两轮 OTA,确认 ota_0/ota_1 切换与 confirm 日志正确。
> 我也清楚边界:当前是现场 push 模型;量产需改为云端 pull(esp_https_ota)+ HTTPS + 签名 + 灰度/anti-rollback。

---

## 六、真机验证记录(本项目)

| 阶段 | 结果 |
|---|---|
| v1.0.0 串口烧录(开回滚) | factory 下 confirm 返回 ESP_FAIL(预期,仅日志) |
| OTA 1.0.0 → 1.0.1 | 200 ok → 重启 → 从 ota_0(0x1a0000)启动,confirm valid |
| OTA 1.0.1 → 1.0.2 | 一次设备掉线退 AP(环境问题,非代码);恢复后重推成功,fw_version=1.0.2 |

## 七、仍留白(可选后续)

- 回滚“失败→自动回退”实测(注释掉 main.c 的 confirm 再 OTA)
- HTTPS 云端 pull(esp_https_ota)
- 固件签名 / Secure Boot / Anti-Rollback
- 服务端 version.json + 灰度
- 设备端真实进度上报与新旧版本对比 UI
