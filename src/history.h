#pragma once

#include <Arduino.h>

#include "scale_protocol.h"

namespace history {

bool begin();
bool append(const scale::Record &record, const scale::Profile &profile);
String page(uint32_t before);

}  // namespace history
