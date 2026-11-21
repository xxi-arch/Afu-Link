#pragma once

#include "scale_protocol.h"

namespace scale {

enum class CompositionMethod : uint8_t { None, Impedance, Bmi };

struct BodyMetric {
  uint16_t deciPercent = 0;
  uint16_t deciKg = 0;
};

struct BodyComposition {
  uint16_t centiBmi = 0;
  CompositionMethod method = CompositionMethod::None;
  uint8_t reserved = 0;
  BodyMetric fat, muscle, skeletalMuscle, water, protein, bone, subcutaneousFat;
};
static_assert(sizeof(BodyComposition) == 32);

BodyComposition estimateBodyComposition(const Record &record, const Profile &profile);

}  // namespace scale
