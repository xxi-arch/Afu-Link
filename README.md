# Afu-Link

基于 ESP32-C3 的莱沃 / Welland 阿福体脂秤 BLE 网关。

当前固件使用 PlatformIO + Arduino，支持：

- 扫描并连接 AFU-WL 系列 BLE 体脂秤。
- 解析 FFB0 / FFB1 / FFB2 通信。
- 本地估算 BMI、体脂率及其他身体组成指标。
- LittleFS 保存测量历史，设备 Web 页面查看记录和维护个人资料。
- ArduinoOTA 固件升级。
- MQTT + Home Assistant MQTT Discovery，自动创建体重和身体组成传感器。

## Home Assistant

MQTT 接入需求、Topic、实体清单、配置步骤和当前限制见：

- [Home Assistant MQTT 接入说明](docs/requirements-home-assistant.md)

HA 需要先配置 MQTT 集成和一个可用的 MQTT Broker（例如 Mosquitto Broker）。Afu-Link 不要求安装第三方 HA 前端卡片；它提供原生 MQTT Sensor 实体，可直接使用 HA 自带的 Tile、Sensor、Gauge、History 等卡片。

## 构建

1. 将 `src/secrets.example.h` 复制为 `src/secrets.h`，填写 Wi-Fi SSID、Wi-Fi 密码和 OTA 密码。
2. 使用 PlatformIO：
   - 环境：`usb`（串口烧录）
   - 环境：`ota`（网络 OTA）
3. OTA 构建 / 上传时，在运行 PlatformIO 的环境中设置 `AFU_OTA_PASSWORD`，使其与 `src/secrets.h` 中的 OTA 密码一致。

首次构建会通过 PlatformIO 安装 PubSubClient 依赖。

## 注意

- 当前体成分数值由固件中的估算算法计算，不代表医疗测量结果，也不保证与蚂蚁阿福 App 的官方算法完全一致。
- 当前 MQTT 使用 TCP 明文连接，建议仅在可信局域网内使用。
- 发布前请检查并移除源码及 PlatformIO 配置中的个人 Wi-Fi / OTA 凭据；公开仓库中的旧凭据应及时轮换。
