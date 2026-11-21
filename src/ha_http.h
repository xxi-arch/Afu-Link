#pragma once

#include "body_composition.h"
#include "scale_protocol.h"

namespace ha_http {

struct Config {
  bool enabled = false;
  char url[128]{};
  char token[256]{};
};

enum class SaveResult { Saved, InvalidUrl, MissingToken, InvalidToken, StorageUnavailable };

bool begin();
Config getConfig();
SaveResult saveConfig(const Config &config);
bool enqueueMeasurement(const scale::Record &record, const scale::Profile &profile);
void loop();
const char *status();

}  // namespace ha_http
