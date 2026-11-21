#pragma once

#include "scale_protocol.h"

namespace ha_mqtt {

struct Config {
  bool enabled = false;
  char host[64]{};
  uint16_t port = 1883;
  char username[32]{};
  char password[64]{};
  char baseTopic[96] = "afulink/afu-a1";
  char discoveryPrefix[32] = "homeassistant";
};

bool begin();
Config getConfig();
bool saveConfig(const Config &config);
bool enqueueMeasurement(const scale::Record &record, const scale::Profile &profile);
void loop();
const char *status();

}  // namespace ha_mqtt
