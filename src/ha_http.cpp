#include "ha_http.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cctype>
#include <ctime>
#include <cstring>

#include "history.h"

namespace ha_http {
namespace {

constexpr uint8_t kQueueLength = 8;
constexpr uint32_t kApiCheckIntervalMs = 30000;
constexpr uint32_t kEntityCheckIntervalMs = 60000;
constexpr uint32_t kRequestIntervalMs = 120;
constexpr uint32_t kRetryIntervalMs = 5000;
constexpr uint16_t kHttpTimeoutMs = 2500;

struct PendingMeasurement {
  scale::Record record;
  scale::BodyComposition body;
  uint32_t nonce = 0;
};

struct SensorDefinition {
  const char *id;
  const char *name;
  const char *unit;
  const char *deviceClass;
  const char *stateClass;
};

constexpr SensorDefinition kSensors[] = {
    {"weight", "Afu-Link 体重", "kg", "weight", "measurement"},
    {"bmi", "Afu-Link BMI", "kg/m²", nullptr, "measurement"},
    {"body_fat_percent", "Afu-Link 体脂率", "%", nullptr, "measurement"},
    {"body_fat_mass", "Afu-Link 脂肪重量", "kg", "weight", "measurement"},
    {"muscle_percent", "Afu-Link 肌肉率", "%", nullptr, "measurement"},
    {"muscle_mass", "Afu-Link 肌肉量", "kg", "weight", "measurement"},
    {"skeletal_muscle_percent", "Afu-Link 骨骼肌率", "%", nullptr, "measurement"},
    {"skeletal_muscle_mass", "Afu-Link 骨骼肌量", "kg", "weight", "measurement"},
    {"water_percent", "Afu-Link 水分率", "%", nullptr, "measurement"},
    {"water_mass", "Afu-Link 水分量", "kg", "weight", "measurement"},
    {"protein_percent", "Afu-Link 蛋白质率", "%", nullptr, "measurement"},
    {"protein_mass", "Afu-Link 蛋白质量", "kg", "weight", "measurement"},
    {"bone_percent", "Afu-Link 骨量率", "%", nullptr, "measurement"},
    {"bone_mass", "Afu-Link 骨量", "kg", "weight", "measurement"},
    {"subcutaneous_fat_percent", "Afu-Link 皮下脂肪率", "%", nullptr, "measurement"},
    {"subcutaneous_fat_mass", "Afu-Link 皮下脂肪量", "kg", "weight", "measurement"},
    {"measurement_time", "Afu-Link 最近称重时间", nullptr, "timestamp", nullptr},
};

Config currentConfig;
QueueHandle_t pendingQueue = nullptr;
PendingMeasurement activeMeasurement{};
bool activeValid = false;
uint8_t activeSensor = 0;
bool apiReady = false;
uint32_t lastApiCheck = 0;
uint32_t lastEntityCheck = 0;
uint32_t lastRequestAt = 0;
uint32_t requestDelayMs = kRequestIntervalMs;
char currentStatus[64] = "未配置";

bool validUrl(const char *value) {
  constexpr char prefix[] = "http://";
  if (strncmp(value, prefix, sizeof(prefix) - 1) != 0) return false;
  const char *host = value + sizeof(prefix) - 1;
  size_t length = strlen(host);
  while (length && host[length - 1] == '/') --length;
  if (!length) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(host[i]);
    if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
  }
  return true;
}

bool validConfig(const Config &config) {
  if (strnlen(config.url, sizeof(config.url)) >= sizeof(config.url) ||
      strnlen(config.token, sizeof(config.token)) >= sizeof(config.token)) return false;
  if (!config.enabled) return true;
  if (!validUrl(config.url) || !config.token[0]) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(config.token); *p; ++p) {
    if (*p < 0x20 || *p == 0x7f) return false;
  }
  return true;
}

void setStatus(const char *value) {
  snprintf(currentStatus, sizeof(currentStatus), "%s", value);
}

void setHttpStatus(int code) {
  if (code == 401) setStatus("Token 无效或已过期");
  else if (code == 403) setStatus("需要 HA 管理员 Token");
  else if (code == 404) setStatus("HA API 路径错误");
  else if (code == 503) setStatus("HA 正在重启或维护");
  else if (code < 0) setStatus("无法连接 HA，请检查地址和网络");
  else {
    snprintf(currentStatus, sizeof(currentStatus), "HA 返回错误：%d", code);
  }
}

String makeUrl(const char *path) {
  String result(currentConfig.url);
  while (result.endsWith("/")) result.remove(result.length() - 1);
  result += path;
  return result;
}

int request(const char *path, const String *payload = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClient transport;
  HTTPClient http;
  const String url = makeUrl(path);
  if (!http.begin(transport, url)) return -1;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.addHeader("Authorization", String("Bearer ") + currentConfig.token);
  int code;
  if (payload) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(*payload);
  } else {
    code = http.GET();
  }
  http.end();
  return code;
}

String measuredAt(const scale::Record &record) {
  if (!record.time) return String();
  const time_t timestamp = static_cast<time_t>(record.time);
  struct tm utc {};
  if (!gmtime_r(&timestamp, &utc)) return String();
  char value[32];
  snprintf(value, sizeof(value), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return String(value);
}

String measurementId(const PendingMeasurement &measurement) {
  char value[48];
  snprintf(value, sizeof(value), "%lu-%lu-%08lx",
           static_cast<unsigned long>(measurement.record.time),
           static_cast<unsigned long>(measurement.record.grams),
           static_cast<unsigned long>(measurement.nonce));
  return String(value);
}

String sensorState(const PendingMeasurement &measurement, uint8_t index) {
  const auto &record = measurement.record;
  const auto &body = measurement.body;
  const bool hasBody = body.method != scale::CompositionMethod::None;
  switch (index) {
    case 0: return String(record.centiKg / 100.0, 2);
    case 1: return hasBody ? String(body.centiBmi / 100.0, 2) : String("unknown");
    case 2: return hasBody ? String(body.fat.deciPercent / 10.0, 1) : String("unknown");
    case 3: return hasBody ? String(body.fat.deciKg / 10.0, 1) : String("unknown");
    case 4: return hasBody ? String(body.muscle.deciPercent / 10.0, 1) : String("unknown");
    case 5: return hasBody ? String(body.muscle.deciKg / 10.0, 1) : String("unknown");
    case 6: return hasBody ? String(body.skeletalMuscle.deciPercent / 10.0, 1) : String("unknown");
    case 7: return hasBody ? String(body.skeletalMuscle.deciKg / 10.0, 1) : String("unknown");
    case 8: return hasBody ? String(body.water.deciPercent / 10.0, 1) : String("unknown");
    case 9: return hasBody ? String(body.water.deciKg / 10.0, 1) : String("unknown");
    case 10: return hasBody ? String(body.protein.deciPercent / 10.0, 1) : String("unknown");
    case 11: return hasBody ? String(body.protein.deciKg / 10.0, 1) : String("unknown");
    case 12: return hasBody ? String(body.bone.deciPercent / 10.0, 1) : String("unknown");
    case 13: return hasBody ? String(body.bone.deciKg / 10.0, 1) : String("unknown");
    case 14: return hasBody ? String(body.subcutaneousFat.deciPercent / 10.0, 1) : String("unknown");
    case 15: return hasBody ? String(body.subcutaneousFat.deciKg / 10.0, 1) : String("unknown");
    case 16: return measuredAt(record).length() ? measuredAt(record) : String("unknown");
  }
  return String("unknown");
}

String buildStatePayload(const PendingMeasurement &measurement, uint8_t index) {
  const auto &sensor = kSensors[index];
  const String value = sensorState(measurement, index);
  const String id = measurementId(measurement);
  const String timestamp = measuredAt(measurement.record);
  const char *method = measurement.body.method == scale::CompositionMethod::Impedance ? "impedance" :
                       measurement.body.method == scale::CompositionMethod::Bmi ? "bmi" : "unknown";

  String json;
  json.reserve(420);
  json = "{\"state\":\"";
  json += value;
  json += "\",\"attributes\":{\"friendly_name\":\"";
  json += sensor.name;
  json += "\"";
  if (sensor.unit) {
    json += ",\"unit_of_measurement\":\"";
    json += sensor.unit;
    json += "\"";
  }
  if (sensor.deviceClass) {
    json += ",\"device_class\":\"";
    json += sensor.deviceClass;
    json += "\"";
  }
  if (sensor.stateClass) {
    json += ",\"state_class\":\"";
    json += sensor.stateClass;
    json += "\"";
  }
  json += ",\"measurement_id\":\"";
  json += id;
  json += "\",\"measured_at\":";
  if (timestamp.length()) {
    json += '\"';
    json += timestamp;
    json += '\"';
  } else {
    json += "null";
  }
  json += ",\"measurement_method\":\"";
  json += method;
  json += "\"";
  if (index == 16) {
    json += ",\"algorithm\":";
    json += String(measurement.record.algorithm);
    json += ",\"adc\":[";
    for (size_t i = 0; i < measurement.record.adcCount && i < scale::kAdcLimit; ++i) {
      if (i) json += ',';
      json += String(measurement.record.adc[i]);
    }
    json += ']';
  }
  json += "}}";
  return json;
}

int publishSensor(const PendingMeasurement &measurement, uint8_t index) {
  char path[96];
  snprintf(path, sizeof(path), "/api/states/sensor.afu_link_%s", kSensors[index].id);
  const String payload = buildStatePayload(measurement, index);
  return request(path, &payload);
}

bool enqueue(const scale::Record &record, const scale::BodyComposition &body) {
  if (!pendingQueue) return false;
  PendingMeasurement pending{};
  pending.record = record;
  pending.body = body;
  pending.nonce = esp_random();
  return xQueueSend(pendingQueue, &pending, 0) == pdPASS;
}

void restoreLatestMeasurement() {
  if (!pendingQueue || uxQueueMessagesWaiting(pendingQueue)) return;
  scale::Record record;
  scale::BodyComposition body;
  if (history::latest(record, body)) {
    if (enqueue(record, body)) Serial.println("HA: queued latest measurement for entity restore");
  }
}

}  // namespace

bool begin() {
  currentConfig = Config{};
  Preferences prefs;
  if (prefs.begin("ha_http", true)) {
    currentConfig.enabled = prefs.getBool("enabled", false);
    prefs.getString("url", currentConfig.url, sizeof(currentConfig.url));
    prefs.getString("token", currentConfig.token, sizeof(currentConfig.token));
    prefs.end();
  }
  if (!validConfig(currentConfig)) currentConfig.enabled = false;
  pendingQueue = xQueueCreate(kQueueLength, sizeof(PendingMeasurement));
  setStatus(currentConfig.enabled ? "等待连接 HA" : "未启用");
  return pendingQueue != nullptr;
}

Config getConfig() {
  return currentConfig;
}

bool saveConfig(const Config &value) {
  Config next = value;
  next.url[sizeof(next.url) - 1] = '\0';
  next.token[sizeof(next.token) - 1] = '\0';
  if (!validConfig(next)) return false;

  Preferences prefs;
  if (!prefs.begin("ha_http", false)) return false;
  prefs.putBool("enabled", next.enabled);
  prefs.putString("url", next.url);
  prefs.putString("token", next.token);
  prefs.end();

  currentConfig = next;
  apiReady = false;
  lastApiCheck = 0;
  lastEntityCheck = 0;
  lastRequestAt = 0;
  requestDelayMs = kRequestIntervalMs;
  activeValid = false;
  activeSensor = 0;
  setStatus(currentConfig.enabled ? "配置已保存，等待测试" : "未启用");
  return true;
}

bool testConnection() {
  if (!currentConfig.enabled) {
    setStatus("未启用");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("设备 Wi-Fi 未连接");
    return false;
  }
  const int code = request("/api/");
  lastApiCheck = millis();
  apiReady = code == 200;
  if (apiReady) {
    setStatus("HA 连接成功");
    lastEntityCheck = 0;
    return true;
  }
  setHttpStatus(code);
  return false;
}

bool enqueueMeasurement(const scale::Record &record, const scale::Profile &profile) {
  if (!currentConfig.enabled) return true;
  const auto body = scale::estimateBodyComposition(record, profile);
  if (enqueue(record, body)) return true;
  Serial.println("HA: upload queue full; measurement remains in local history");
  return false;
}

void loop() {
  if (!pendingQueue || !currentConfig.enabled) return;
  if (WiFi.status() != WL_CONNECTED) {
    apiReady = false;
    lastApiCheck = 0;
    setStatus("设备 Wi-Fi 未连接");
    return;
  }

  if (!lastApiCheck || millis() - lastApiCheck >= kApiCheckIntervalMs) {
    const int code = request("/api/");
    lastApiCheck = millis();
    if (code != 200) {
      apiReady = false;
      setHttpStatus(code);
      return;
    }
    apiReady = true;
    setStatus("HA 已连接");
  }
  if (!apiReady) return;

  if (!lastEntityCheck || millis() - lastEntityCheck >= kEntityCheckIntervalMs) {
    lastEntityCheck = millis();
    const int code = request("/api/states/sensor.afu_link_weight");
    if (code == 404) {
      restoreLatestMeasurement();
    } else if (code == 401 || code == 403) {
      apiReady = false;
      setHttpStatus(code);
      return;
    } else if (code != 200) {
      setHttpStatus(code);
      return;
    }
  }

  if (lastRequestAt && millis() - lastRequestAt < requestDelayMs) return;
  if (!activeValid) {
    if (xQueuePeek(pendingQueue, &activeMeasurement, 0) != pdPASS) return;
    activeValid = true;
    activeSensor = 0;
  }

  const int code = publishSensor(activeMeasurement, activeSensor);
  lastRequestAt = millis();
  if (code >= 200 && code < 300) {
    requestDelayMs = kRequestIntervalMs;
    ++activeSensor;
    if (activeSensor >= sizeof(kSensors) / sizeof(kSensors[0])) {
      PendingMeasurement completed{};
      xQueueReceive(pendingQueue, &completed, 0);
      activeValid = false;
      activeSensor = 0;
      setStatus("上传成功");
      Serial.println("HA: measurement uploaded via REST API");
    }
    return;
  }

  requestDelayMs = kRetryIntervalMs;
  setHttpStatus(code);
  if (code == 401 || code == 403 || code < 0 || code == 503) apiReady = false;
}

const char *status() {
  if (!currentConfig.enabled) return "未启用";
  if (WiFi.status() != WL_CONNECTED) return "设备 Wi-Fi 未连接";
  return currentStatus;
}

}  // namespace ha_http
