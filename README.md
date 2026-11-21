# Afu-Link

基于 ESP32-C3 的莱沃 / Welland 阿福体脂秤 BLE 网关。

当前固件使用 PlatformIO + Arduino，支持：

- 扫描并连接 AFU-WL 系列 BLE 体脂秤。
- 解析 FFB0 / FFB1 / FFB2 通信。
- 本地估算 BMI、体脂率及其他身体组成指标。
- LittleFS 保存测量历史，设备 Web 页面查看记录和维护个人资料。
- 每次蓝牙连接接收秤的 D8 历史通知，按测量时间补录并跳过已有记录。网页删除会在 ESP32-C3 保存标记，后续同步仍会跳过该记录。
- ArduinoOTA 固件升级。
- 通过 Home Assistant REST API 上传体重和身体组成数据，自动创建 17 个状态实体。

## Home Assistant 接入

打开 Afu-Link Web 页面，展开「Home Assistant 接入」：

1. 填写 Home Assistant 根地址，例如 `http://192.168.1.10:8123`。
2. 在 HA 个人资料 → 安全 → 长期访问令牌中创建令牌，粘贴到 Token 输入框。
3. 勾选启用自动上传，点击「保存并测试连接」。

不需要安装 Mosquitto、配置 MQTT Broker、Topic 或 HACS。Afu-Link 使用 HA REST API 的 `POST /api/states/<entity_id>` 创建 / 更新实体。

实体包括体重、BMI、体脂率、脂肪重量、肌肉率 / 量、骨骼肌率 / 量、水分率 / 量、蛋白质率 / 量、骨量率 / 量、皮下脂肪率 / 量和最近称重时间。

**REST API 方式的限制：**

- 这些是 HA 运行期间的动态状态实体，不会像 MQTT Discovery 一样注册为一个完整的设备条目；可以用于 HA 仪表盘和自动化。
- HA 重启后动态实体会消失。Afu-Link 会检测体重实体是否存在，若不存在则从 LittleFS 恢复最近一次测量并重新创建实体。
- 当前只支持局域网 `http://` 地址，暂不支持 HTTPS。Token 会保存在 ESP32-C3 NVS 中；HTTP 不加密，请勿通过公网或不可信网络使用。
- Token 需要有写入实体状态的管理员权限；建议为设备单独创建长期访问令牌。

## 构建

1. 将 `src/secrets.example.h` 复制为 `src/secrets.h`，填写 Wi-Fi SSID、Wi-Fi 密码和 OTA 密码。
2. 使用 PlatformIO：
   - 环境：`usb`（串口烧录）
   - 环境：`ota`（网络 OTA）
3. OTA 构建 / 上传时，在运行 PlatformIO 的环境中设置 `AFU_OTA_PASSWORD`，使其与 `src/secrets.h` 中的 OTA 密码一致。

网页源码在 `web/`，包括 `index.html`、`styles.css`、`app.js` 和电子秤图标 `scale.png`。修改这些文件后，重新构建并上传固件即可更新网页。

合宙 LuatOS ESP32C3-CORE 的 D4（GPIO12）在固件启动后常亮。接通体脂秤的蓝牙通知时 D5（GPIO13）也亮，断开后 D5 熄灭。板载灯为单色、高电平点亮，Flash 使用 DIO 模式。 [合宙板卡文档](https://wiki-zh.luatos.org/chips/esp32c3/board.html)

## 注意

- 当前体成分数值由固件中的估算算法计算，不代表医疗测量结果，也不保证与蚂蚁阿福 App 的官方算法完全一致。
- 上传队列最多缓存 8 条测量，暂存于 RAM；断电前未成功上传的记录仍保存在 C3 的 LittleFS 历史中，但不会自动逐条补传。
- 公开仓库历史中曾包含 Wi-Fi / OTA 凭据，即使当前源码已改为本地 secrets 文件，也应轮换旧密码。
