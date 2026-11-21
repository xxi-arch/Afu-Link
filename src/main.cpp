#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdlib>
#include <ctime>

#include "history.h"
#include "ha_http.h"
#include "scale_ble.h"
#include "web_page.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef AFU_WIFI_SSID
#define AFU_WIFI_SSID ""
#endif
#ifndef AFU_WIFI_PASSWORD
#define AFU_WIFI_PASSWORD ""
#endif
#ifndef AFU_OTA_PASSWORD
#define AFU_OTA_PASSWORD ""
#endif

namespace {
constexpr char kHostname[] = "afu-a1";
constexpr char kWifiSsid[] = AFU_WIFI_SSID;
constexpr char kWifiPassword[] = AFU_WIFI_PASSWORD;
constexpr char kOtaPassword[] = AFU_OTA_PASSWORD;

WebServer web(80);
bool servicesRunning = false;

uint32_t readNumber(const char *key, uint32_t maximum) {
  const String text = web.arg(key);
  if (!text.length() || text.length() > 6) return 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
  }
  const auto value = strtoul(text.c_str(), nullptr, 10);
  return value <= maximum ? value : 0;
}

void sendProfile() {
  const auto profile = scale::getProfile();
  char json[64];
  snprintf(json, sizeof(json), "{\"age\":%u,\"height\":%u,\"sex\":%u}",
           profile.age, profile.heightCm, profile.sex);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", json);
}

void receiveProfile() {
  const scale::Profile profile{
      uint8_t(readNumber("age", 120)), uint8_t(readNumber("height", 230)), uint8_t(readNumber("sex", 2))};
  if (!profile.valid()) {
    web.send(400, "application/json", "{\"error\":\"invalid_profile\"}");
  } else if (!scale::saveProfile(profile)) {
    web.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
  } else {
    sendProfile();
  }
}

String jsonEscape(const char *value) {
  String result;
  for (const char *p = value; *p; ++p) {
    if (*p == 34 || *p == 92) result += static_cast<char>(92);
    if (static_cast<uint8_t>(*p) >= 0x20) result += *p;
  }
  return result;
}

bool copyArgument(const char *name, char *destination, size_t capacity) {
  const String value = web.arg(name);
  if (value.length() >= capacity) return false;
  value.toCharArray(destination, capacity);
  return true;
}

void sendHaConfig() {
  const auto config = ha_http::getConfig();
  String json = R"({"enabled":)";
  json += config.enabled ? "true" : "false";
  json += R"(,"url":")";
  json += jsonEscape(config.url);
  json += '"';
  json += R"(,"tokenSet":)";
  json += config.token[0] ? "true" : "false";
  json += R"(,"status":")";
  json += jsonEscape(ha_http::status());
  json += '"';
  json += '}';
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", json);
}

void receiveHaConfig() {
  auto config = ha_http::getConfig();
  config.enabled = web.arg("enabled") == "1" || web.arg("enabled") == "true";
  if (!copyArgument("url", config.url, sizeof(config.url))) {
    web.send(400, "application/json", R"({"error":"invalid_url"})");
    return;
  }
  if (web.arg("clearToken") == "1") {
    config.token[0] = '\0';
  } else if (web.arg("token").length() &&
             !copyArgument("token", config.token, sizeof(config.token))) {
    web.send(400, "application/json", R"({"error":"token_too_long"})");
    return;
  }
  if (!ha_http::saveConfig(config)) {
    web.send(400, "application/json", R"({"error":"invalid_config"})");
    return;
  }
  if (config.enabled) ha_http::testConnection();
  sendHaConfig();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("AFU scale receiver");

  Serial.println(history::begin() ? "Scale: storage ready" : "Scale: storage unavailable");
  Serial.println(ha_http::begin() ? "HA: REST queue ready" : "HA: REST queue unavailable");
  scale::beginReceiver();

  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", kWebPage); });
  web.on("/api/measurements", HTTP_GET, [] {
    String json = history::page(readNumber("before", 999999));
    json.remove(json.length() - 1);
    json += F(",\"ble\":\"");
    json += scale::receiverStatus();
    json += F("\",\"clockSynced\":");
    json += time(nullptr) >= scale::kClockReady ? "true" : "false";
    json += '}';
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", json);
  });
  web.on("/api/profile", HTTP_GET, sendProfile);
  web.on("/api/profile", HTTP_POST, receiveProfile);
  web.on("/api/ha", HTTP_GET, sendHaConfig);
  web.on("/api/ha", HTTP_POST, receiveHaConfig);
  ArduinoOTA.setHostname(kHostname);
  ArduinoOTA.setPassword(kOtaPassword);

  WiFi.persistent(false);
  WiFi.setHostname(kHostname);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(kWifiSsid, kWifiPassword);
}

void loop() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != servicesRunning) {
    if (connected) {
      configTime(0, 0, "ntp.aliyun.com", "time.cloudflare.com");
      web.begin();
      ArduinoOTA.begin();
    } else {
      ArduinoOTA.end();
      web.stop();
    }
    servicesRunning = connected;
  }

  if (servicesRunning) {
    ArduinoOTA.handle();
    web.handleClient();
  }
  ha_http::loop();
  delay(2);
}
