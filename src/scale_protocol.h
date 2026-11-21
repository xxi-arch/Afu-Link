#pragma once

#include <cstddef>
#include <cstdint>

namespace scale {

constexpr size_t kAdcLimit = 5;
constexpr size_t kPacketSize = 20;
constexpr uint32_t kClockReady = 1704067200;

struct Profile {
  uint8_t age = 0;
  uint8_t heightCm = 0;
  uint8_t sex = 0;

  bool valid() const;
};

enum class RecordSource : uint8_t { Live, ScaleHistory };

struct Record {
  uint32_t time = 0;
  uint32_t grams = 0;
  uint16_t centiKg = 0;
  uint16_t adc[kAdcLimit]{};
  uint8_t adcCount = 0;
  uint8_t algorithm = 0;
  uint8_t heightCm = 0;
  RecordSource source = RecordSource::Live;  // Uses a byte reserved in v1/v2 records.
  uint8_t reserved[4]{};
  uint32_t checksum = 0;
};
static_assert(sizeof(Record) == 32);

enum class FrameType { None, Weight, Adc, History };

struct Frame {
  FrameType type = FrameType::None;
  uint32_t time = 0;
  uint32_t grams = 0;
  uint16_t centiKg = 0;
  bool stable = false;
  uint8_t algorithm = 0;
  uint8_t adcCount = 0;
  uint16_t adc[kAdcLimit]{};
};

// AFU-WL scale27 GATT frames described by the reference project.
Frame decode(const uint8_t *data, size_t length);
void encodeProfile(uint8_t (&packet)[kPacketSize], uint32_t time, const Profile &profile);
bool sameMeasurement(const Record &left, const Record &right);

class Capture {
 public:
  void feed(const Frame &frame, uint32_t nowMs, uint32_t time, const Profile &profile);
  bool take(Record &output, Profile &profile, uint32_t nowMs, bool disconnected = false);

 private:
  void setWeight(const Frame &frame, uint32_t nowMs, uint32_t time, const Profile &profile);

  Record pending_{};
  Profile pendingProfile_{};
  uint32_t stableAt_ = 0;
  uint32_t lastPacketAt_ = 0;
  uint8_t movingFrames_ = 0;
  bool ready_ = false;
  bool captured_ = false;
};

}  // namespace scale
