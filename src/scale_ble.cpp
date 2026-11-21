#include "scale_ble.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <Preferences.h>

#include <atomic>
#include <ctime>

#include "history.h"
#include "ha_http.h"

namespace scale {
namespace {
const BLEUUID kService(static_cast<uint16_t>(0xffb0));
const BLEUUID kWrite(static_cast<uint16_t>(0xffb1));
const BLEUUID kNotify(static_cast<uint16_t>(0xffb2));
constexpr uint32_t kIdleDisconnectMs = 25000;

enum class State { Starting, Scanning, Connecting, Receiving, Error };
std::atomic<State> state{State::Starting};
std::atomic<uint32_t> storedProfile{0};
QueueHandle_t frames = nullptr;

uint32_t pack(const Profile &profile) {
  return uint32_t(profile.age) | (uint32_t(profile.heightCm) << 8) | (uint32_t(profile.sex) << 16);
}

class Finder : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice device) override {
    if (!device.isConnectable() || !device.getName().startsWith("AFU-WL") || device.getRSSI() <= rssi) return;

    const String manufacturer = device.getManufacturerData();
    const bool wellandManufacturer = manufacturer.length() >= 2 && uint8_t(manufacturer[0]) == 0xac;
    if (wellandManufacturer && uint8_t(manufacturer[1]) != 0x27) return;
    bool match = wellandManufacturer;
    for (int i = 0; !match && i < device.getServiceDataUUIDCount(); ++i) {
      match = device.getServiceDataUUID(i) == BLEUUID(static_cast<uint16_t>(0x27ac));
    }
    if (!match && !device.isAdvertisingService(kService)) return;

    address = device.getAddress();
    rssi = device.getRSSI();
  }

  BLEAddress address;
  int rssi = -128;
};

void onFrame(BLERemoteCharacteristic *, uint8_t *data, size_t size, bool) {
  const Frame frame = decode(data, size);
  if (frame.type != FrameType::None && xQueueSend(frames, &frame, 0) != pdPASS) {
    // Preserve room for the most recent result if a burst fills the queue.
    Frame oldest;
    xQueueReceive(frames, &oldest, 0);
    xQueueSend(frames, &frame, 0);
  }
}

uint32_t now() {
  const time_t value = time(nullptr);
  return value >= kClockReady ? static_cast<uint32_t>(value) : 0;
}

void store(Capture &capture, bool disconnected = false) {
  Record record;
  Profile profile;
  if (capture.take(record, profile, millis(), disconnected)) {
    if (!record.time) record.time = now();
    const bool saved = history::append(record, profile);
    Serial.println(saved ? "Scale: measurement saved" : "Scale: storage write failed");
    if (!ha_http::enqueueMeasurement(record, profile)) {
      Serial.println("HA: measurement queued unsuccessfully");
    }
  }
}

void readSession(BLEClient &client, Capture &capture) {
  xQueueReset(frames);
  client.getServices();
  auto *service = client.getService(kService);
  if (!service) service = client.getService(BLEUUID(kService).to128());
  if (!service) return;
  auto *notify = service->getCharacteristic(kNotify);
  if (!notify) notify = service->getCharacteristic(BLEUUID(kNotify).to128());
  if (!notify || !notify->canNotify() || !notify->subscribe(true, onFrame)) return;
  state = State::Receiving;
  Serial.println("Scale: subscribed to FFB2");

  auto *writer = service->getCharacteristic(kWrite);
  if (!writer) writer = service->getCharacteristic(BLEUUID(kWrite).to128());
  uint32_t sentProfile = 0;
  uint32_t lastFrame = millis();
  do {
    const Profile profile = getProfile();
    const uint32_t profileValue = pack(profile);
    const uint32_t clock = now();
    if (writer && profile.valid() && clock && profileValue != sentProfile) {
      uint8_t packet[kPacketSize];
      encodeProfile(packet, clock, profile);
      sentProfile = profileValue;
      if (!writer->writeValue(packet, sizeof(packet), !writer->canWriteNoResponse())) {
        Serial.println("Scale: could not send profile");
      }
    }
    Frame frame;
    if (xQueueReceive(frames, &frame, pdMS_TO_TICKS(80)) == pdPASS) {
      lastFrame = millis();
      capture.feed(frame, lastFrame, clock, profile);
    }
    store(capture);
  } while (client.isConnected() && millis() - lastFrame < kIdleDisconnectMs);

  Frame remaining;
  const Profile profile = getProfile();
  while (xQueueReceive(frames, &remaining, 0) == pdPASS) {
    capture.feed(remaining, millis(), now(), profile);
    store(capture);
  }
  store(capture, true);
}

void receiver(void *) {
  BLEDevice::init("");
  auto *client = BLEDevice::createClient();
  if (!client) {
    state = State::Error;
    vTaskDelete(nullptr);
    return;
  }
  auto *scan = BLEDevice::getScan();
  Finder finder;
  scan->setAdvertisedDeviceCallbacks(&finder);
  scan->setActiveScan(true);
  scan->setInterval(150);
  scan->setWindow(70);
  scan->setDuplicateFilter(true);
  Capture capture;
  BLEAddress activeScale;
  for (;;) {
    state = State::Scanning;
    finder.rssi = -128;
    scan->start(2, false);
    scan->clearResults();
    if (finder.rssi != -128) {
      state = State::Connecting;
      if (client->connect(finder.address)) {
        if (finder.address != activeScale) {
          activeScale = finder.address;
          capture = {};
        }
        readSession(*client, capture);
        if (client->isConnected()) client->disconnect();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
}  // namespace

Profile getProfile() {
  const uint32_t stored = storedProfile.load();
  return {uint8_t(stored), uint8_t(stored >> 8), uint8_t(stored >> 16)};
}

bool saveProfile(const Profile &profile) {
  if (!profile.valid()) return false;
  const uint32_t value = pack(profile);
  if (value == storedProfile.load()) return true;
  Preferences prefs;
  if (!prefs.begin("scale")) return false;
  const size_t written = prefs.putUInt("profile", value);
  if (written != sizeof(value)) return false;
  storedProfile = value;
  return true;
}

bool beginReceiver() {
  Preferences prefs;
  if (prefs.begin("scale", true)) {
    storedProfile = prefs.getUInt("profile", 0);
    prefs.end();
  }
  frames = xQueueCreate(32, sizeof(Frame));
  if (frames && xTaskCreate(receiver, "scale-rx", 6144, nullptr, 1, nullptr) == pdPASS) return true;
  state = State::Error;
  return false;
}

const char *receiverStatus() {
  switch (state.load()) {
    case State::Starting: return "starting";
    case State::Scanning: return "scanning";
    case State::Connecting: return "connecting";
    case State::Receiving: return "receiving";
    case State::Error: return "error";
  }
  return "error";
}

}  // namespace scale
