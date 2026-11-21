#include "history.h"

#include <LittleFS.h>
#include <cerrno>
#include <esp_crc.h>
#include <esp_partition.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "body_composition.h"

namespace history {
namespace {
constexpr char kLegacyPath[] = "/littlefs/measure-v1.bin";
constexpr char kPath[] = "/littlefs/measure-v2.bin";
constexpr size_t kPageSize = 24;
constexpr uint8_t kErasedByte = 0xff;

struct SavedMeasurement {
  scale::Record record;
  scale::BodyComposition body;
};
static_assert(sizeof(SavedMeasurement) == 64);

SemaphoreHandle_t access = nullptr;
uint32_t legacyCount = 0;
uint32_t count = 0;
bool writable = false;
bool mounted = false;
bool opened = false;

bool emptyPartition() {
  const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition) return false;
  uint8_t buffer[256];
  // LittleFS keeps its superblock in the first block pair.
  for (size_t offset = 0; offset < 8192; offset += sizeof(buffer)) {
    if (esp_partition_read(partition, offset, buffer, sizeof(buffer)) != ESP_OK) return false;
    for (auto byte : buffer) {
      if (byte != kErasedByte) return false;
    }
  }
  return true;
}

uint32_t checksum(const scale::Record &record) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), offsetof(scale::Record, checksum));
}

uint32_t checksum(const SavedMeasurement &entry) {
  return esp_crc32_le(checksum(entry.record), reinterpret_cast<const uint8_t *>(&entry.body), sizeof(entry.body));
}

bool valid(const scale::Record &record) {
  return record.grams != 0 && record.adcCount <= scale::kAdcLimit;
}

bool verified(const scale::Record &record) {
  return valid(record) && record.checksum == checksum(record);
}

bool verified(const SavedMeasurement &entry) {
  return valid(entry.record) && entry.body.method <= scale::CompositionMethod::Bmi &&
         entry.record.checksum == checksum(entry);
}

template <typename Entry>
bool readEntry(int file, uint32_t index, Entry &entry) {
  if (file < 0) return false;
  const off_t offset = index * sizeof(entry);
  return lseek(file, offset, SEEK_SET) == offset && read(file, &entry, sizeof(entry)) == sizeof(entry) &&
         verified(entry);
}

template <typename Entry>
bool loadCount(int file, uint32_t &quantity) {
  struct stat info {};
  if (fstat(file, &info) != 0) return false;
  quantity = info.st_size / sizeof(Entry);
  Entry tail;
  while (quantity && !readEntry(file, quantity - 1, tail)) --quantity;
  return true;
}

void addMetric(String &json, const char *key, const scale::BodyMetric &metric) {
  char field[54];
  snprintf(field, sizeof(field), ",\"%s\":[%u.%u,%u.%u]", key,
           metric.deciPercent / 10, metric.deciPercent % 10, metric.deciKg / 10, metric.deciKg % 10);
  json += field;
}

void addComposition(String &json, const scale::BodyComposition &body) {
  char head[75];
  snprintf(head, sizeof(head), ",\"body\":{\"method\":\"%s\",\"bmi\":%u.%02u",
           body.method == scale::CompositionMethod::Impedance ? "impedance" : "bmi",
           body.centiBmi / 100, body.centiBmi % 100);
  json += head;
  addMetric(json, "fat", body.fat);
  addMetric(json, "muscle", body.muscle);
  addMetric(json, "skeletalMuscle", body.skeletalMuscle);
  addMetric(json, "water", body.water);
  addMetric(json, "protein", body.protein);
  addMetric(json, "bone", body.bone);
  addMetric(json, "subcutaneousFat", body.subcutaneousFat);
  json += '}';
}

void addJson(String &json, uint32_t id, const scale::Record &record, const scale::BodyComposition *body) {
  char head[145];
  snprintf(head, sizeof(head),
           "{\"id\":%lu,\"time\":%lu,\"grams\":%lu,\"centiKg\":%u,\"heightCm\":%u,\"algorithm\":%u,\"adc\":[",
           static_cast<unsigned long>(id), static_cast<unsigned long>(record.time),
           static_cast<unsigned long>(record.grams), record.centiKg, record.heightCm, record.algorithm);
  json += head;
  for (size_t i = 0; i < record.adcCount; ++i) {
    if (i) json += ',';
    json += record.adc[i];
  }
  json += ']';
  if (!body && record.heightCm) json += F(",\"legacy\":true");
  if (body && body->method != scale::CompositionMethod::None) addComposition(json, *body);
  json += '}';
}
}  // namespace

bool begin() {
  access = xSemaphoreCreateMutex();
  if (!access) return false;
  if (!LittleFS.begin() && (!emptyPartition() || !LittleFS.begin(true))) return false;
  mounted = true;

  bool legacyReady;
  const int legacy = open(kLegacyPath, O_RDONLY);
  if (legacy >= 0) {
    legacyReady = loadCount<scale::Record>(legacy, legacyCount);
    close(legacy);
  } else {
    legacyReady = errno == ENOENT;
  }
  const int file = open(kPath, O_RDWR | O_CREAT, 0600);
  if (file < 0) return false;
  const bool currentReady = loadCount<SavedMeasurement>(file, count);
  close(file);
  opened = legacyReady && currentReady;
  writable = opened;
  return writable;
}

bool append(const scale::Record &record, const scale::Profile &profile) {
  if (!opened) return false;
  SavedMeasurement entry{record, scale::estimateBodyComposition(record, profile)};
  entry.record.checksum = checksum(entry);
  xSemaphoreTake(access, portMAX_DELAY);
  const int file = open(kPath, O_RDWR | O_CREAT, 0600);
  const off_t offset = count * sizeof(entry);
  bool saved = file >= 0 && lseek(file, offset, SEEK_SET) == offset &&
               write(file, &entry, sizeof(entry)) == sizeof(entry) && fsync(file) == 0;
  if (file >= 0 && close(file) != 0) saved = false;
  if (saved) ++count;
  writable = saved;
  xSemaphoreGive(access);
  return saved;
}

String page(uint32_t before) {
  if (!mounted) return F("{\"writable\":false,\"count\":0,\"next\":0,\"rows\":[]}");
  String json;
  json.reserve(9800);
  xSemaphoreTake(access, portMAX_DELAY);
  const uint32_t total = legacyCount + count;
  json = F("{\"writable\":");
  json += writable ? "true" : "false";
  json += F(",\"count\":");
  json += total;
  json += F(",\"rows\":[");
  uint32_t id = before && before <= total ? before - 1 : total;
  const int legacyFile = legacyCount ? open(kLegacyPath, O_RDONLY) : -1;
  const int file = id > legacyCount ? open(kPath, O_RDONLY) : -1;
  bool comma = false;
  for (size_t i = 0; i < kPageSize && id; ++i, --id) {
    if (id > legacyCount) {
      SavedMeasurement entry;
      if (!readEntry(file, id - legacyCount - 1, entry)) continue;
      if (comma) json += ',';
      addJson(json, id, entry.record, &entry.body);
    } else {
      scale::Record record;
      if (!readEntry(legacyFile, id - 1, record)) continue;
      if (comma) json += ',';
      addJson(json, id, record, nullptr);
    }
    comma = true;
  }
  if (legacyFile >= 0) close(legacyFile);
  if (file >= 0) close(file);
  json += F("],\"next\":");
  json += id ? id + 1 : 0;
  json += '}';
  xSemaphoreGive(access);
  return json;
}

}  // namespace history
