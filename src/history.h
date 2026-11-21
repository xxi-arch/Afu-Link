#pragma once

#include <Arduino.h>

#include "body_composition.h"
#include "scale_protocol.h"

namespace history {

bool begin();
bool append(const scale::Record &record, const scale::Profile &profile);
bool latest(scale::Record &record, scale::BodyComposition &body);
String page(uint32_t before);

}  // namespace history
