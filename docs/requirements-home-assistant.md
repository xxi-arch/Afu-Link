# Home Assistant REST API 接入需求与使用说明

## 1. 目标

为 Afu-Link 增加低门槛的 Home Assistant 上传方式：用户只需填写 HA 根地址和长期访问令牌，不需要安装 MQTT Broker 或理解 Topic / Discovery。

采用 Home Assistant REST API：

- `GET /api/` 用于验证地址和 Token。
- `POST /api/states/<entity_id>` 创建或更新传感器状态。
- ESP32-C3 只在一轮称重完成后上传，不上传 BLE Notify 中间帧。
- 本地 LittleFS 历史记录继续独立工作，HA 不可用时不影响称重采集。

## 2. 配置界面

Web 页面「Home Assistant 接入」仅保留：

1. Home Assistant 地址，例如 `http://192.168.1.10:8123`。
2. 长期访问令牌（Token）。
3. 启用自动上传。
4. 保存并测试连接按钮。

Token 在 HA 个人资料 → 安全 → 长期访问令牌中创建。需要使用具备写入实体状态权限的管理员账户。

Token 保存在 ESP32-C3 NVS；GET 配置接口只返回 `tokenSet`，不返回 Token 明文。保存时 Token 输入留空表示保留已保存 Token；勾选清除可删除。

当前 URL 仅接受局域网 HTTP 根地址，不接受 HTTPS 或带路径的反向代理 URL。

## 3. 传感器实体

首次上传后，HA 动态创建以下状态实体：

| Entity ID | 名称 | 单位 |
|---|---|---:|
| `sensor.afu_link_weight` | Afu-Link 体重 | kg |
| `sensor.afu_link_bmi` | Afu-Link BMI | kg/m² |
| `sensor.afu_link_body_fat_percent` | Afu-Link 体脂率 | % |
| `sensor.afu_link_body_fat_mass` | Afu-Link 脂肪重量 | kg |
| `sensor.afu_link_muscle_percent` | Afu-Link 肌肉率 | % |
| `sensor.afu_link_muscle_mass` | Afu-Link 肌肉量 | kg |
| `sensor.afu_link_skeletal_muscle_percent` | Afu-Link 骨骼肌率 | % |
| `sensor.afu_link_skeletal_muscle_mass` | Afu-Link 骨骼肌量 | kg |
| `sensor.afu_link_water_percent` | Afu-Link 水分率 | % |
| `sensor.afu_link_water_mass` | Afu-Link 水分量 | kg |
| `sensor.afu_link_protein_percent` | Afu-Link 蛋白质率 | % |
| `sensor.afu_link_protein_mass` | Afu-Link 蛋白质量 | kg |
| `sensor.afu_link_bone_percent` | Afu-Link 骨量率 | % |
| `sensor.afu_link_bone_mass` | Afu-Link 骨量 | kg |
| `sensor.afu_link_subcutaneous_fat_percent` | Afu-Link 皮下脂肪率 | % |
| `sensor.afu_link_subcutaneous_fat_mass` | Afu-Link 皮下脂肪量 | kg |
| `sensor.afu_link_measurement_time` | Afu-Link 最近称重时间 | — |

体成分无法计算时，对应实体状态为 `unknown`，体重仍会正常上传。

每个实体均带有 `measurement_id`、`measured_at` 和 `measurement_method` 属性；最近称重时间实体还带有算法编号和原始 ADC 数组。

## 4. 上传与重复处理

- 只有现有 `Capture::take()` 产出完整记录时才进入上传队列。
- 每次测量生成唯一 `measurement_id`。即使体重数值相同，测量 ID 属性变化也能让 HA 识别为新测量。
- 一次测量依次更新 17 个实体；请求间隔约 120 ms。
- HTTP 失败时保留当前测量并重试；RAM 队列最多 8 条。断电前未成功上传的数据不会自动从历史逐条补传。
- 设备每 30 秒检查 API 可用性，并每 60 秒检查 `sensor.afu_link_weight` 是否存在。若 HA 重启导致动态实体消失，会从 LittleFS 读取最近一次测量并重新创建实体。

## 5. 使用与限制

1. 确保 ESP32-C3 与 Home Assistant 位于可互相访问的家庭局域网。
2. 打开 Afu-Link Web 页面（默认 `http://afu-a1.local`）。
3. 填写 HA 根地址和管理员长期访问令牌，启用并保存。
4. 页面显示「HA 连接成功」后，下一次完成称重即可上传。
5. 可在 HA「开发者工具 → 状态」中查看 `sensor.afu_link_*`，并在仪表盘中添加 Tile、实体、Gauge、History 等官方卡片。

重要限制：

- REST API 动态状态实体不会像 MQTT Discovery 那样注册为完整的 HA 设备条目；实体可用于仪表盘和自动化，但设备列表中不会出现 Afu-Link 设备卡片。
- HA 重启后实体会暂时消失，设备会在下一轮检测中恢复最近一次数据。
- 当前只支持 HTTP，不支持 HTTPS；Bearer Token 会在局域网内以明文 HTTP 传输。请勿将设备端口或 HA API 暴露到公网。
- 使用管理员 Token。401 表示 Token 无效 / 过期；403 表示当前用户没有写入状态的权限。

## 6. 构建与安全

复制 `src/secrets.example.h` 为 `src/secrets.h`，填写 Wi-Fi 和 OTA 凭据后使用 PlatformIO 构建。

公开仓库历史中曾包含 Wi-Fi / OTA 凭据，即使当前源码已移至本地 secrets 文件，也应轮换旧密码。
