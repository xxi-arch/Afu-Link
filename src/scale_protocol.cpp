#include "scale_protocol.h"

namespace scale {
namespace {
constexpr uint32_t kAdcWaitMs = 18000;
constexpr uint32_t kNewWakeAfterMs = 12000;

uint16_t displayWeight(uint32_t grams, uint8_t division) {
  switch (division) {
    case 0: return (grams + 5) / 10;
    case 1: return ((grams + (grams % 10 == 9 ? 10 : 0)) / 10 + 1) & ~1u;
    case 2: return ((grams + 20) / 50) * 5;
    case 4: {
      const uint32_t decagrams = grams / 10;
      return (((decagrams + (decagrams % 10 == 9 ? 10 : 0)) / 10 + 1) & ~1u) * 10;
    }
    default: return ((grams + 50) / 100) * 10;
  }
}

void readWeight(Frame &frame, const uint8_t *data) {
  const uint32_t encoded = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                           (uint32_t(data[2]) << 8) | data[3];
  frame.grams = encoded & 0x3ffff;
  frame.centiKg = displayWeight(frame.grams, (encoded >> 18) & 7);
  frame.stable = (encoded >> 31) != 0;
}
}  // namespace

bool Profile::valid() const {
  return age >= 5 && age <= 120 && heightCm >= 80 && heightCm <= 230 && (sex == 1 || sex == 2);
}

Frame decode(const uint8_t *data, size_t length) {
  Frame frame{};
  if (length != kPacketSize || data[0] != 0xac || (data[1] & 0x80) != 0) return frame;
  uint8_t checksum = 0;
  for (size_t i = 2; i < 19; ++i) checksum += data[i];
  if ((checksum & 0x1f) != (data[19] & 0x1f)) return frame;

  frame.algorithm = data[17];
  if (data[18] == 0xd5) {
    frame.type = FrameType::Weight;
    readWeight(frame, data + 2);
  } else if (data[18] == 0xd6) {
    const uint8_t count = data[2];
    if (count == 0 || count > kAdcLimit) return {};
    const size_t modeOffset = 4 + 2 * count;
    frame.type = FrameType::Adc;
    frame.adcCount = count;
    for (size_t i = 0; i < count; ++i) {
      frame.adc[i] = (uint16_t(data[4 + 2 * i]) << 8) | data[5 + 2 * i];
    }
    if (data[modeOffset] == 1 && modeOffset + 4 <= 17) readWeight(frame, data + modeOffset + 1);
  }
  return frame;
}

void encodeProfile(uint8_t (&packet)[kPacketSize], uint32_t time, const Profile &profile) {
  for (auto &byte : packet) byte = 0;
  packet[0] = 0xac;
  packet[1] = 0x27;
  for (size_t i = 0; i < 4; ++i) packet[2 + i] = time >> (24 - 8 * i);
  packet[6] = 32;  // UTC+8 in quarter-hour units.
  packet[8] = 1;   // User slot.
  packet[9] = profile.heightCm;
  packet[12] = profile.age;
  packet[13] = profile.sex;
  packet[16] = 3;  // Enable impedance measurement.
  packet[18] = 0xd0;
  for (size_t i = 2; i < 19; ++i) packet[19] += packet[i];
}

void Capture::setWeight(const Frame &frame, uint32_t nowMs, uint32_t time, const Profile &profile) {
  if (!pending_.grams) {
    stableAt_ = nowMs;
    pendingProfile_ = profile.valid() ? profile : Profile{};
    pending_.heightCm = pendingProfile_.heightCm;
  }
  if (!pending_.time) pending_.time = time;
  pending_.grams = frame.grams;
  pending_.centiKg = frame.centiKg;
  pending_.algorithm = frame.algorithm;
}

void Capture::feed(const Frame &frame, uint32_t nowMs, uint32_t time, const Profile &profile) {
  if (frame.type == FrameType::None) return;
  if (captured_ && nowMs - lastPacketAt_ >= kNewWakeAfterMs) captured_ = false;
  lastPacketAt_ = nowMs;

  if (frame.type == FrameType::Weight && (!frame.stable || frame.grams == 0)) {
    if (captured_ && (frame.grams == 0 || ++movingFrames_ >= 3)) captured_ = false;
    if (pending_.grams && frame.grams == 0) ready_ = true;
    return;
  }
  movingFrames_ = 0;
  if (captured_ || ready_) return;
  if (frame.type == FrameType::Weight) {
    setWeight(frame, nowMs, time, profile);
  } else {
    if (frame.grams != 0) setWeight(frame, nowMs, time, profile);
    if (!pending_.grams) return;
    pending_.algorithm = frame.algorithm;
    pending_.adcCount = frame.adcCount;
    for (size_t i = 0; i < frame.adcCount; ++i) pending_.adc[i] = frame.adc[i];
    ready_ = true;
  }
}

bool Capture::take(Record &output, Profile &profile, uint32_t nowMs, bool disconnected) {
  if (!pending_.grams || (!ready_ && !disconnected && nowMs - stableAt_ < kAdcWaitMs)) return false;
  output = pending_;
  profile = pendingProfile_;
  pending_ = {};
  pendingProfile_ = {};
  ready_ = false;
  captured_ = true;
  return true;
}

}  // namespace scale
