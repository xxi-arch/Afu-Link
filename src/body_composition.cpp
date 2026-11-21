#include "body_composition.h"

#include <algorithm>

namespace scale {
namespace {
double primaryResistance(const Record &record) {
  // The Welland D6 notification contains dedicated ADC samples. Prefer its first usable channel.
  for (size_t i = 0; i < std::min<size_t>(record.adcCount, kAdcLimit); ++i) {
    const uint16_t raw = record.adc[i];
    const double ohms = raw >= 1500 ? raw * 0.88 : raw;
    if (ohms >= 100 && ohms <= 1500) return ohms;
  }
  return 0;
}

BodyMetric metric(double percent, double mass) {
  return {static_cast<uint16_t>(percent * 10 + 0.5), static_cast<uint16_t>(mass * 10 + 0.5)};
}
}  // namespace

BodyComposition estimateBodyComposition(const Record &record, const Profile &profile) {
  if (!profile.valid() || !record.centiKg) return {};

  // Coefficients and display precision from smart-body-scale-IOS (BodyAlgorithm, BodyMeasurement).
  const double weight = record.centiKg / 100.0;
  const double height = profile.heightCm / 100.0;
  const double bmi = weight / (height * height);
  const double resistance = primaryResistance(record);
  const double age = profile.age;
  const bool male = profile.sex == 1;
  double fat;
  if (resistance > 0) {
    fat = male ? 0.18 * bmi + 0.012 * age + 0.018 * resistance - 3.2
               : 0.26 * bmi + 0.011 * age + 0.020 * resistance - 2.5;
  } else {
    fat = 1.20 * bmi + 0.23 * age - (male ? 10.8 : 0.0) - 5.4;
  }
  fat = std::clamp(fat, 5.0, 55.0);

  const double water = std::clamp(69.7 - fat * 0.55, 35.0, 75.0);
  const double protein = std::clamp(16.0 + (water - 50) * 0.12, 10.0, 24.0);
  const double bone = std::min(weight, std::clamp(weight * (male ? 0.047 : 0.040), 1.5, 5.5));
  const double muscle = std::max(weight * (1 - fat / 100) - bone, 0.0);
  const double subcutaneousFat = fat * 0.72;

  BodyComposition result;
  result.centiBmi = static_cast<uint16_t>(bmi * 100 + 0.5);
  result.method = resistance > 0 ? CompositionMethod::Impedance : CompositionMethod::Bmi;
  result.fat = metric(fat, weight * fat / 100);
  result.muscle = metric(100 * muscle / weight, muscle);
  result.skeletalMuscle = metric(100 * muscle / weight * 0.526, muscle * 0.526);
  result.water = metric(water, weight * water / 100);
  result.protein = metric(protein, weight * protein / 100);
  result.bone = metric(100 * bone / weight, bone);
  result.subcutaneousFat = metric(subcutaneousFat, weight * subcutaneousFat / 100);
  return result;
}

}  // namespace scale
