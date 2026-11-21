#pragma once

#include <Arduino.h>

#include "body_composition.h"
#include "scale_protocol.h"

namespace history {

enum class AppendResult { Saved, Duplicate, Failed };
enum class DeleteResult { Deleted, NotFound, Failed };

bool begin();
AppendResult append(const scale::Record &record, const scale::Profile &profile);
DeleteResult hide(uint32_t id);
bool latest(scale::Record &record, scale::BodyComposition &body);
String page(uint32_t before);

}  // namespace history
