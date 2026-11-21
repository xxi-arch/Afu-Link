# Home Assistant MQTT 接入需求与使用说明

## 1. 目标

在不改变现有 BLE 采集、体成分计算、LittleFS 历史记录和设备 Web 页面功能的前提下，为 Afu-Link 增加 Home Assistant 接入能力。

采用 MQTT，而不是让 ESP32-C3 直接调用 Home Assistant REST API：

- ESP32-C3 只需要连接 MQTT Broker，不保存 Home Assistant Long-Lived Access Token。
- 使用 MQTT Discovery 自动在 HA 中创建传感器实体。
- 体重秤 BLE 断开、Wi-Fi 或 Broker 暂时不可用时，不影响本地测量记录。
- MQTT 配置通过设备 Web 页面维护，并保存到 ESP32 NVS。

## 2. 功能需求

### 2.1 MQTT 配置

设备 Web 页面增加「Home Assistant（MQTT）」设置区，支持：

- 启用 / 禁用上传。
- Broker 主机名或 IPv4 地址。
- Broker 端口，默认 1883。
- 可选用户名和密码。
- 数据 Topic 前缀，默认 `afulink/afu-a1`。
- HA Discovery 前缀，默认 `homeassistant`。
- 配置保存到 NVS，重启后保留。
- 密码不通过 GET API 返回；密码输入留空时保留已保存密码，可主动清除密码。
- 显示 MQTT 当前连接状态。

### 2.2 Home Assistant 自动发现

MQTT 连接成功后，自动向 Discovery Topic 发布 retained 配置。HA 已配置 MQTT 集成时，应自动创建一个名为「Afu-Link」的设备，不需要手工创建 MQTT Sensor。

设备信息：

- 设备名：Afu-Link
- 制造商：xxi-arch
- 型号：ESP32-C3 BLE scale gateway
- 标识：afu_link_afu_a1

### 2.3 传感器实体

| 指标 | MQTT JSON 字段 | 单位 | HA 类型 |
|---|---|---:|---|
| 体重 | `weight_kg` | kg | weight |
| BMI | `bmi` | kg/m² | measurement |
| 体脂率 | `body_fat_percent` | % | measurement |
| 脂肪重量 | `body_fat_kg` | kg | weight |
| 肌肉率 | `muscle_percent` | % | measurement |
| 肌肉量 | `muscle_kg` | kg | weight |
| 骨骼肌率 | `skeletal_muscle_percent` | % | measurement |
| 骨骼肌量 | `skeletal_muscle_kg` | kg | weight |
| 水分率 | `water_percent` | % | measurement |
| 水分量 | `water_kg` | kg | weight |
| 蛋白质率 | `protein_percent` | % | measurement |
| 蛋白质量 | `protein_kg` | kg | weight |
| 骨量率 | `bone_percent` | % | measurement |
| 骨量 | `bone_kg` | kg | weight |
| 皮下脂肪率 | `subcutaneous_fat_percent` | % | measurement |
| 皮下脂肪量 | `subcutaneous_fat_kg` | kg | weight |
| 最近称重时间 | `measured_at` | — | timestamp |

如果用户资料不完整，或体成分算法无法计算，体成分相关传感器发布为 `unknown`，体重仍正常上报。

### 2.4 Topic 约定

默认 Topic：

| Topic | 用途 | Retain |
|---|---|---|
| `homeassistant/sensor/afu_link_<sensor>/config` | HA Discovery 配置 | 是 |
| `afulink/afu-a1/state` | 最新一次测量 JSON | 是 |
| `afulink/afu-a1/availability` | 设备在线状态（online/offline） | 是 |

Discovery 前缀和数据前缀均可在 Web 页面修改。

状态 JSON 包含：

- `measurement_id`：本次测量的唯一标识。
- `measured_at`：UTC ISO-8601 时间。
- `measurement_method`：阻抗估算、BMI 估算或 unknown。
- 体重、BMI、体成分指标。
- 原始 ADC 数组和算法编号，便于后续诊断。

## 3. 重复测量与离线处理

- 现有 `Capture` 状态机负责识别一轮称重；只有 `Capture::take()` 产出完整记录时才入队，不会把每一帧 BLE Notify 都上传。
- 每次测量生成独立 `measurement_id`，即使两次体重数值相同也能区分。
- 状态 Topic 使用 retained，HA 重启后能读取最近一次测量。
- 测量传感器启用 `force_update`，即使连续两次测量的数值或时间相同，也会按每次完成的测量触发状态更新。
- 最近称重时间传感器每次测量都会更新，并暴露本次 JSON 数据作为实体属性，可用于 HA 自动化读取测量 ID、算法和 ADC。
- MQTT 发布采用 PubSubClient 的 QoS 0。设备运行期间有最多 8 条测量的 RAM 待发送队列；Broker 暂时断开时会等待重连。
- 当前版本没有将待发送队列持久化到 LittleFS。若测量后设备在成功上传前断电，该条记录仍保留在 C3 本地历史中，但不会自动补传。
- 如果两次测量的体重数值完全相同，HA 的体重实体数值状态可能不产生新的数值变化；「最近称重时间」实体仍会更新。C3 本地历史保留每次测量。

## 4. 使用步骤

1. 在 Home Assistant 安装并启动 Mosquitto Broker，或准备其他 MQTT Broker。
2. 在 HA「设置 → 设备与服务」中添加 / 配置 MQTT 集成，确认 Broker 可正常连接。
3. 确保 ESP32-C3 和 Broker 位于可互相访问的网络中。
4. 打开 Afu-Link Web 页面（默认可通过 `http://afu-a1.local` 访问）。
5. 展开「Home Assistant（MQTT）」设置区，填写 Broker 的局域网 IP / 主机名、端口和可选账号密码。
6. 勾选启用上传并保存。
7. 等待 Web 页面显示「MQTT 已连接」。完成下一次称重后，HA 会收到传感器状态。
8. 在 HA「设置 → 设备与服务 → MQTT → 设备」中找到 Afu-Link。

注意：ESP32-C3 应填写 Broker 对局域网开放的地址。不要把容器内部服务名当作 ESP32 可解析的主机名；使用 HA 主机或 Broker 的局域网 IP 通常最稳妥。

## 5. 验收标准

- 未配置 MQTT 时，BLE 采集、体成分计算、本地历史和现有 Web 页面保持可用。
- MQTT 配置保存后，重启设备仍然保留。
- MQTT 连接成功后，HA 自动发现 Afu-Link 设备和上述传感器。
- 完成一次称重后，状态 Topic 收到一条包含本次测量 ID 和时间的 JSON。
- 同一轮称重的多帧 BLE Notify 不会产生多条 MQTT 测量。
- Broker 暂时不可用时，本地历史仍正常保存；Broker 恢复后，设备运行期间的待发送队列继续上传。
- HA 重启后可通过 retained Discovery 和 retained state 恢复设备配置及最近一次状态。

## 6. 当前版本边界与后续建议

当前实现使用明文 MQTT TCP（默认 1883），没有 TLS；MQTT 密码保存在 ESP32 NVS 中。设备 Web 配置页目前没有登录认证，应仅在可信家庭局域网访问。

Wi-Fi 和 OTA 凭据已改为从本地忽略文件 `src/secrets.h` 读取，复制 `src/secrets.example.h` 后填写。由于旧凭据曾存在于公开仓库历史中，即使当前分支删除了明文，也应立即轮换 Wi-Fi 和 OTA 密码。

建议后续迭代：

1. 将待上传测量队列持久化到 LittleFS，支持断电补传。
2. 增加 MQTT TLS / CA 证书配置。
3. 为设备 Web 管理页面增加访问认证。
4. 增加多成员档案与成员识别；当前固件只有一个本地用户档案。
5. 增加上传成功计数、最后上传时间和最近错误原因。
