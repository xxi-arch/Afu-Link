#include "mqtt_ha.h"

#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_system.h>

#include <atomic>
#include <cctype>
#include <ctime>
#include <cstring>

#include "body_composition.h"

namespace ha_mqtt {
namespace {

constexpr char kDeviceId[] = "afu_link";
constexpr char kDeviceName[] = "Afu-Link";
constexpr char kFirmwareVersion[] = "0.2.0";
constexpr uint8_t kQueueLength = 8;
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kPublishIntervalMs = 250;

struct PendingMeasurement {
  scale::Record record;
  scale::Profile profile;
  uint32_t nonce;
};

struct SensorDefinition {
  const char *id;
  const char *name;
  const char *key;
  const char *unit;
  const char *deviceClass;
  const char *stateClass;
};

constexpr SensorDefinition kSensors[] = {
    {"weight", "体重", "weight_kg", "kg", "weight", "measurement"},
    {"bmi", "BMI", "bmi", "kg/m²", nullptr, "measurement"},
    {"body_fat_percent", "体脂率", "body_fat_percent", "%", nullptr, "measurement"},
    {"body_fat_mass", "脂肪重量", "body_fat_kg", "kg", "weight", "measurement"},
    {"muscle_percent", "肌肉率", "muscle_percent", "%", nullptr, "measurement"},
    {"muscle_mass", "肌肉量", "muscle_kg", "kg", "weight", "measurement"},
    {"skeletal_muscle_percent", "骨骼肌率", "skeletal_muscle_percent", "%", nullptr, "measurement"},
    {"skeletal_muscle_mass", "骨骼肌量", "skeletal_muscle_kg", "kg", "weight", "measurement"},
    {"water_percent", "水分率", "water_percent", "%", nullptr, "measurement"},
    {"water_mass", "水分量", "water_kg", "kg", "weight", "measurement"},
    {"protein_percent", "蛋白质率", "protein_percent", "%", nullptr, "measurement"},
    {"protein_mass", "蛋白质量", "protein_kg", "kg", "weight", "measurement"},
    {"bone_percent", "骨量率", "bone_percent", "%", nullptr, "measurement"},
    {"bone_mass", "骨量", "bone_kg", "kg", "weight", "measurement"},
    {"subcutaneous_fat_percent", "皮下脂肪率", "subcutaneous_fat_percent", "%", nullptr, "measurement"},
    {"subcutaneous_fat_mass", "皮下脂肪量", "subcutaneous_fat_kg", "kg", "weight", "measurement"},
    {"measurement_time", "最近称重时间", "measured_at", nullptr, "timestamp", nullptr},
};

WiFiClient network;
PubSubClient client(network);
QueueHandle_t pendingQueue = nullptr;
Config currentConfig;
bool discoveryPublished = false;
bool reconnectRequested = false;
std::atomic<bool> uploadEnabled{false};
uint32_t lastConnectAttempt = 0;
uint32_t lastDiscoveryAttempt = 0;
uint32_t lastPublishAt = 0;

bool validTopic(const char *value, size_t capacity) {
  const size_t length = strnlen(value, capacity);
  if (!length || length >= capacity || value[0] == '/' || value[length - 1] == '/') return false;
  bool previousSlash = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    const bool slash = c == '/';
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || slash)) return false;
    if (slash && previousSlash) return false;
    previousSlash = slash;
  }
  return true;
}

bool validHost(const char *value, size_t capacity) {
  const size_t length = strnlen(value, capacity);
  if (!length || length >= capacity) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':')) return false;
  }
  return true;
}

bool validConfig(const Config &value) {
  if (!value.port || !validTopic(value.baseTopic, sizeof(value.baseTopic)) ||
      !validTopic(value.discoveryPrefix, sizeof(value.discoveryPrefix))) return false;
  if (value.enabled && !validHost(value.host, sizeof(value.host))) return false;
  if (strnlen(value.username, sizeof(value.username)) >= sizeof(value.username) ||
      strnlen(value.password, sizeof(value.password)) >= sizeof(value.password)) return false;
  return true;
}

String topic(const char *suffix) {
  String result(currentConfig.baseTopic);
  result += '/';
  result += suffix;
  return result;
}

void appendNumber(String &json, const char *key, double value, uint8_t decimals, bool available = true) {
  json += ",\"";
  json += key;
  json += "\":";
  if (available) json += String(value, static_cast<unsigned int>(decimals));
  else json += "null";
}

void appendBodyMetric(String &json, const char *percentKey, const char *massKey,
                      const scale::BodyMetric &metric, bool available) {
  appendNumber(json, percentKey, metric.deciPercent / 10.0, 1, available);
  appendNumber(json, massKey, metric.deciKg / 10.0, 1, available);
}

String buildPayload(const PendingMeasurement &pending) {
  const auto body = scale::estimateBodyComposition(pending.record, pending.profile);
  const bool hasBody = body.method != scale::CompositionMethod::None;
  const char *method = body.method == scale::CompositionMethod::Impedance ? "impedance" :
                       body.method == scale::CompositionMethod::Bmi ? "bmi" : "unknown";

  char measuredAt[32] = {};
  if (pending.record.time) {
    const time_t timestamp = static_cast<time_t>(pending.record.time);
    struct tm utc {};
    if (gmtime_r(&timestamp, &utc)) {
      snprintf(measuredAt, sizeof(measuredAt), "%04d-%02d-%02dT%02d:%02d:%02dZ",
               utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
               utc.tm_hour, utc.tm_min, utc.tm_sec);
    }
  }

  char measurementId[40];
  snprintf(measurementId, sizeof(measurementId), "%lu-%lu-%08lx",
           static_cast<unsigned long>(pending.record.time),
           static_cast<unsigned long>(pending.record.grams),
           static_cast<unsigned long>(pending.nonce));

  String json;
  json.reserve(760);
  json = "{\"measurement_id\":\"";
  json += measurementId;
  json += "\",\"measured_at\":";
  if (measuredAt[0]) {
    json += '"';
    json += measuredAt;
    json += '"';
  } else {
    json += "null";
  }
  json += ",\"measurement_method\":\"";
  json += method;
  json += "\"";
  appendNumber(json, "weight_kg", pending.record.centiKg / 100.0, 2);
  appendNumber(json, "weight_g", pending.record.grams, 0);
  appendNumber(json, "bmi", body.centiBmi / 100.0, 2, hasBody);
  appendBodyMetric(json, "body_fat_percent", "body_fat_kg", body.fat, hasBody);
  appendBodyMetric(json, "muscle_percent", "muscle_kg", body.muscle, hasBody);
  appendBodyMetric(json, "skeletal_muscle_percent", "skeletal_muscle_kg", body.skeletalMuscle, hasBody);
  appendBodyMetric(json, "water_percent", "water_kg", body.water, hasBody);
  appendBodyMetric(json, "protein_percent", "protein_kg", body.protein, hasBody);
  appendBodyMetric(json, "bone_percent", "bone_kg", body.bone, hasBody);
  appendBodyMetric(json, "subcutaneous_fat_percent", "subcutaneous_fat_kg", body.subcutaneousFat, hasBody);
  json += ",\"algorithm\":";
  json += String(pending.record.algorithm);
  json += ",\"adc\":[";
  for (size_t i = 0; i < pending.record.adcCount && i < scale::kAdcLimit; ++i) {
    if (i) json += ',';
    json += String(pending.record.adc[i]);
  }
  json += "]}";
  return json;
}

bool publishDiscovery() {
  const String stateTopic = topic("state");
  const String availabilityTopic = topic("availability");

  for (const auto &sensor : kSensors) {
    char discoveryTopic[192];
    snprintf(discoveryTopic, sizeof(discoveryTopic), "%s/sensor/%s_%s/config",
             currentConfig.discoveryPrefix, kDeviceId, sensor.id);

    String payload;
    payload.reserve(640);
    payload = "{\"name\":\"";
    payload += sensor.name;
    payload += "\",\"unique_id\":\"";
    payload += kDeviceId;
    payload += '_';
    payload += sensor.id;
    payload += "\",\"state_topic\":\"";
    payload += stateTopic;
    payload += "\",\"value_template\":\"{{ value_json.";
    payload += sensor.key;
    payload += " if value_json.";
    payload += sensor.key;
    payload += " is not none else 'unknown' }}\"";
    if (sensor.unit) {
      payload += ",\"unit_of_measurement\":\"";
      payload += sensor.unit;
      payload += '"';
    }
    if (sensor.deviceClass) {
      payload += ",\"device_class\":\"";
      payload += sensor.deviceClass;
      payload += '"';
    }
    if (sensor.stateClass) {
      payload += ",\"state_class\":\"";
      payload += sensor.stateClass;
      payload += '"';
    }
    payload += ",\"force_update\":true";
    if (strcmp(sensor.id, "measurement_time") == 0) {
      payload += ",\"json_attributes_topic\":\"";
      payload += stateTopic;
      payload += '"';
    }
    payload += ",\"availability_topic\":\"";
    payload += availabilityTopic;
    payload += "\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";
    payload += ",\"device\":{\"identifiers\":[\"afu_link_afu_a1\"],\"name\":\"";
    payload += kDeviceName;
    payload += "\",\"manufacturer\":\"xxi-arch\",\"model\":\"ESP32-C3 BLE scale gateway\",\"sw_version\":\"";
    payload += kFirmwareVersion;
    payload += "\"},\"origin\":{\"name\":\"Afu-Link\",\"sw_version\":\"";
    payload += kFirmwareVersion;
    payload += "\"}}";

    if (!client.publish(discoveryTopic, payload.c_str(), true)) {
      Serial.printf("HA: discovery publish failed for %s\n", sensor.id);
      return false;
    }
  }
  return true;
}

bool publishMeasurement(const PendingMeasurement &pending) {
  const String stateTopic = topic("state");
  const String payload = buildPayload(pending);
  if (!client.publish(stateTopic.c_str(), payload.c_str(), true)) {
    Serial.println("HA: measurement publish failed");
    return false;
  }
  Serial.println("HA: measurement uploaded");
  return true;
}

void loadConfig() {
  currentConfig = Config{};
  Preferences prefs;
  if (!prefs.begin("ha_mqtt", true)) return;
  currentConfig.enabled = prefs.getBool("enabled", false);
  currentConfig.port = prefs.getUShort("port", 1883);
  prefs.getString("host", currentConfig.host, sizeof(currentConfig.host));
  prefs.getString("username", currentConfig.username, sizeof(currentConfig.username));
  prefs.getString("password", currentConfig.password, sizeof(currentConfig.password));
  prefs.getString("base_topic", currentConfig.baseTopic, sizeof(currentConfig.baseTopic));
  prefs.getString("discovery", currentConfig.discoveryPrefix, sizeof(currentConfig.discoveryPrefix));
  prefs.end();
  if (!validConfig(currentConfig)) currentConfig.enabled = false;
  uploadEnabled.store(currentConfig.enabled && currentConfig.host[0]);
}

}  // namespace

bool begin() {
  loadConfig();
  pendingQueue = xQueueCreate(kQueueLength, sizeof(PendingMeasurement));
  client.setBufferSize(1536);
  client.setKeepAlive(30);
  client.setSocketTimeout(3);
  return pendingQueue != nullptr;
}

Config getConfig() {
  return currentConfig;
}

bool saveConfig(const Config &value) {
  Config next = value;
  next.host[sizeof(next.host) - 1] = '\0';
  next.username[sizeof(next.username) - 1] = '\0';
  next.password[sizeof(next.password) - 1] = '\0';
  next.baseTopic[sizeof(next.baseTopic) - 1] = '\0';
  next.discoveryPrefix[sizeof(next.discoveryPrefix) - 1] = '\0';
  if (!validConfig(next)) return false;

  Preferences prefs;
  if (!prefs.begin("ha_mqtt", false)) return false;
  prefs.putBool("enabled", next.enabled);
  prefs.putUShort("port", next.port);
  prefs.putString("host", next.host);
  prefs.putString("username", next.username);
  prefs.putString("password", next.password);
  prefs.putString("base_topic", next.baseTopic);
  prefs.putString("discovery", next.discoveryPrefix);
  prefs.end();

  if (client.connected()) {
    const String oldAvailabilityTopic = topic("availability");
    client.publish(oldAvailabilityTopic.c_str(), "offline", true);
    client.disconnect();
  }
  currentConfig = next;
  uploadEnabled.store(currentConfig.enabled && currentConfig.host[0]);
  reconnectRequested = true;
  discoveryPublished = false;
  return true;
}

bool enqueueMeasurement(const scale::Record &record, const scale::Profile &profile) {
  if (!pendingQueue) return false;
  if (!uploadEnabled.load()) return true;
  PendingMeasurement pending{};
  pending.record = record;
  pending.profile = profile;
  pending.nonce = esp_random();
  if (xQueueSend(pendingQueue, &pending, 0) == pdPASS) return true;
  Serial.println("HA: upload queue full; measurement kept in local history only");
  return false;
}

void loop() {
  if (!pendingQueue) return;
  if (reconnectRequested) {
    if (client.connected()) client.disconnect();
    reconnectRequested = false;
    discoveryPublished = false;
    lastConnectAttempt = 0;
  }

  if (!currentConfig.enabled || !currentConfig.host[0]) {
    if (client.connected()) {
      const String availabilityTopic = topic("availability");
      client.publish(availabilityTopic.c_str(), "offline", true);
      client.disconnect();
    }
    discoveryPublished = false;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (client.connected()) client.disconnect();
    discoveryPublished = false;
    return;
  }

  if (!client.connected()) {
    discoveryPublished = false;
    if (millis() - lastConnectAttempt < kReconnectIntervalMs) return;
    lastConnectAttempt = millis();

    client.setServer(currentConfig.host, currentConfig.port);
    const String availabilityTopic = topic("availability");
    uint8_t mac[6]{};
    WiFi.macAddress(mac);
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "afu-link-%02x%02x%02x", mac[3], mac[4], mac[5]);

    const bool connected = currentConfig.username[0]
        ? client.connect(clientId, currentConfig.username, currentConfig.password,
                         availabilityTopic.c_str(), 0, true, "offline")
        : client.connect(clientId, availabilityTopic.c_str(), 0, true, "offline");
    if (!connected) {
      Serial.printf("HA: MQTT connection failed, state=%d\n", client.state());
      return;
    }
    client.publish(availabilityTopic.c_str(), "online", true);
    Serial.println("HA: MQTT connected");
  }

  client.loop();

  if (!discoveryPublished) {
    if (millis() - lastDiscoveryAttempt < kReconnectIntervalMs) return;
    lastDiscoveryAttempt = millis();
    discoveryPublished = publishDiscovery();
    if (discoveryPublished) Serial.println("HA: discovery published");
    return;
  }

  if (millis() - lastPublishAt < kPublishIntervalMs) return;
  PendingMeasurement pending{};
  if (xQueuePeek(pendingQueue, &pending, 0) != pdPASS) return;
  if (publishMeasurement(pending)) {
    xQueueReceive(pendingQueue, &pending, 0);
    lastPublishAt = millis();
  }
}

const char *status() {
  if (!currentConfig.enabled) return "未启用";
  if (!currentConfig.host[0]) return "未配置 Broker";
  if (WiFi.status() != WL_CONNECTED) return "Wi-Fi 未连接";
  if (client.connected()) return "MQTT 已连接";
  return "MQTT 未连接";
}

}  // namespace ha_mqtt
