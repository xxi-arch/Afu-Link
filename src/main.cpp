#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdlib>
#include <ctime>

#include "history.h"
#include "scale_ble.h"
#include "web_page.h"

namespace {
constexpr char kHostname[] = "afu-a1";
constexpr char kWifiSsid[] = "esp";
constexpr char kWifiPassword[] = "qwert1234";

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
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("AFU scale receiver");

  Serial.println(history::begin() ? "Scale: storage ready" : "Scale: storage unavailable");
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
  ArduinoOTA.setHostname(kHostname);
  ArduinoOTA.setPassword(kWifiPassword);

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
  delay(2);
}
