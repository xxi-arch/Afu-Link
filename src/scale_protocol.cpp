#include "scale_protocol.h"

namespace scale {
namespace {
constexpr uint32_t kAdcWaitMs = 18000;
constexpr uint32_t kNewWakeAfterMs = 12000;
constexpr uint32_t kReplayWindowSeconds = 12;

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

uint32_t readUint32(const uint8_t *data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}

void readWeight(Frame &frame, const uint8_t *data) {
  const uint32_t encoded = readUint32(data);
  frame.grams = encoded & 0x3ffff;
  frame.centiKg = displayWeight(frame.grams, (encoded >> 18) & 7);
  frame.stable = (encoded >> 31) != 0;
}

Frame readHistory(const uint8_t *data) {
  Frame frame{};
  frame.time = readUint32(data + 3);
  readWeight(frame, data + 7);
  if (frame.time < kClockReady || !frame.grams) return {};

  size_t adcOffset = 0;
  size_t adcCount = 0;
  size_t afterAdcs = 0;
  switch (data[2]) {
    case 0:  // Heart rate and a count of ADC values.
      adcOffset = 15;
      adcCount = data[14];
      break;
    case 2:  // Remaining-history count followed by the ADC count.
      adcOffset = 13;
      adcCount = data[12];
      break;
    default:  // Fixed pair of ADC values followed by two auxiliary bytes.
      adcOffset = 11;
      adcCount = 2;
      afterAdcs = 2;
      break;
  }

  constexpr size_t payloadEnd = 18;
  if (adcCount > kAdcLimit) return {};
  const size_t available = (payloadEnd - adcOffset - afterAdcs) / 2;
  frame.type = FrameType::History;
  frame.adcCount = adcCount < available ? adcCount : available;
  for (size_t i = 0; i < frame.adcCount; ++i) {
    const size_t offset = adcOffset + 2 * i;
    frame.adc[i] = (uint16_t(data[offset]) << 8) | data[offset + 1];
  }
  const size_t algorithmOffset = adcOffset + 2 * adcCount + afterAdcs;
  frame.algorithm = algorithmOffset < payloadEnd ? data[algorithmOffset] : 0;
  return frame;
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
  } else if (data[18] == 0xd8) {
    return readHistory(data);
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

bool sameMeasurement(const Record &left, const Record &right) {
  const bool leftHistory = left.source == RecordSource::ScaleHistory;
  const bool rightHistory = right.source == RecordSource::ScaleHistory;
  if ((!leftHistory && !rightHistory) || !left.time || !right.time || left.grams != right.grams) return false;
  if (left.time == right.time) return true;
  if (leftHistory == rightHistory) return false;

  const uint32_t difference = left.time > right.time ? left.time - right.time : right.time - left.time;
  if (!left.adcCount || !right.adcCount) return difference <= 2;
  if (difference > kReplayWindowSeconds) return false;
  // Live readings use the C3's receive time. Require a matching ADC for a wider window.
  for (size_t i = 0; i < left.adcCount; ++i) {
    for (size_t j = 0; j < right.adcCount; ++j) {
      if (left.adc[i] && left.adc[i] == right.adc[j]) return true;
    }
  }
  return false;
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
  if (frame.type != FrameType::Weight && frame.type != FrameType::Adc) return;
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
