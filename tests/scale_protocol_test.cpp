#include "body_composition.h"
#include "scale_protocol.h"

#include <cassert>

using namespace scale;

namespace {
// Captures provided with Mzdyl/ant-afu-welland-scale.
constexpr uint8_t kWeight[kPacketSize] = {
    0xac, 0x29, 0x80, 0x68, 0xe2, 0xc2, 0x02, 0x00, 0x05, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0xd5, 0x11};
constexpr uint8_t kAdc[kPacketSize] = {
    0xac, 0x29, 0x02, 0x00, 0x02, 0x05, 0x01, 0xd8, 0x01, 0x80,
    0x68, 0xe2, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x29, 0xd6, 0x0e};

void assertMetric(const BodyMetric &metric, uint16_t percent, uint16_t kg) {
  assert(metric.deciPercent == percent && metric.deciKg == kg);
}
}  // namespace

int main() {
  const Frame weight = decode(kWeight, sizeof(kWeight));
  assert(weight.type == FrameType::Weight && weight.stable && weight.centiKg == 5805);
  assert(weight.grams == 58050 && weight.algorithm == 41);
  const Frame adc = decode(kAdc, sizeof(kAdc));
  assert(adc.type == FrameType::Adc && adc.centiKg == 5805);
  assert(adc.adcCount == 2 && adc.adc[0] == 517 && adc.adc[1] == 472);

  uint8_t corrupt[kPacketSize]{};
  for (size_t i = 0; i < kPacketSize; ++i) corrupt[i] = kAdc[i];
  ++corrupt[19];
  assert(decode(corrupt, sizeof(corrupt)).type == FrameType::None);
  assert(decode(kWeight, kPacketSize - 1).type == FrameType::None);

  const Profile profile{23, 170, 1};
  constexpr uint32_t time = 1700000000;
  Capture capture;
  Record item;
  Profile measuredProfile;
  capture.feed(weight, 1, time, profile);
  capture.feed(weight, 2, time + 1, profile);
  assert(!capture.take(item, measuredProfile, 2));
  capture.feed(adc, 120, time + 2, {35, 165, 2});
  assert(capture.take(item, measuredProfile, 120));
  assert(item.time == time && item.adcCount == 2 && item.heightCm == 170);
  assert(measuredProfile.age == 23 && measuredProfile.heightCm == 170 && measuredProfile.sex == 1);
  const auto body = estimateBodyComposition(item, measuredProfile);
  assert(body.method == CompositionMethod::Impedance && body.centiBmi == 2009);
  assertMetric(body.fat, 100, 58);
  assertMetric(body.muscle, 853, 495);
  assertMetric(body.skeletalMuscle, 449, 260);
  assertMetric(body.water, 642, 373);
  assertMetric(body.protein, 177, 103);
  assertMetric(body.bone, 47, 27);
  assertMetric(body.subcutaneousFat, 72, 42);
  assert(estimateBodyComposition(item, {0, 170, 1}).method == CompositionMethod::None);
  assert(estimateBodyComposition(item, {23, 0, 1}).method == CompositionMethod::None);
  assert(estimateBodyComposition(item, {23, 170, 0}).method == CompositionMethod::None);

  Record missingAdc;
  missingAdc.centiKg = 6520;
  missingAdc.adcCount = 2;
  missingAdc.adc[0] = 0xffff;
  const auto fallback = estimateBodyComposition(missingAdc, {35, 165, 2});
  assert(fallback.method == CompositionMethod::Bmi && fallback.centiBmi == 2395);
  assertMetric(fallback.fat, 314, 205);
  missingAdc.adc[1] = 520;
  const auto secondChannel = estimateBodyComposition(missingAdc, {35, 165, 2});
  assert(secondChannel.method == CompositionMethod::Impedance);
  assertMetric(secondChannel.fat, 145, 95);

  item.adc[0] = 1500;
  assertMetric(estimateBodyComposition(item, profile).fat, 245, 142);
  capture.feed(adc, 121, time + 2, profile);
  capture.feed(weight, 122, time + 2, profile);
  assert(!capture.take(item, measuredProfile, 200, true));

  Frame released{FrameType::Weight};
  capture.feed(released, 300, time + 3, profile);
  capture.feed(weight, 400, time + 4, {});
  assert(capture.take(item, measuredProfile, 401, true));
  assert(item.centiKg == 5805 && item.adcCount == 0 && item.heightCm == 0);
  assert(!measuredProfile.valid());

  capture.feed(weight, 20000, time + 20, {});
  assert(!capture.take(item, measuredProfile, 20001));
  capture.feed(adc, 20002, time + 20, profile);
  assert(capture.take(item, measuredProfile, 20002));
  assert(item.adcCount == 2 && item.heightCm == 0 && !measuredProfile.valid());

  capture.feed(weight, 40000, time + 40, {});
  assert(capture.take(item, measuredProfile, 58000));

  uint8_t packet[kPacketSize];
  encodeProfile(packet, time, profile);
  constexpr uint8_t expected[kPacketSize] = {
      0xac, 0x27, 0x65, 0x53, 0xf1, 0x00, 0x20, 0x00, 0x01, 0xaa,
      0x00, 0x00, 0x17, 0x01, 0x00, 0x00, 0x03, 0x00, 0xd0, 0x5f};
  for (size_t i = 0; i < kPacketSize; ++i) assert(packet[i] == expected[i]);
}
