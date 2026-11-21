#include "history.h"

#include <LittleFS.h>
#include <algorithm>
#include <cerrno>
#include <esp_crc.h>
#include <esp_partition.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "body_composition.h"

namespace history {
namespace {
constexpr char kLegacyPath[] = "/littlefs/measure-v1.bin";
constexpr char kPath[] = "/littlefs/measure-v2.bin";
constexpr char kDeletedPath[] = "/littlefs/measure-deleted.bin";
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
uint32_t deletedCount = 0;
std::unique_ptr<uint8_t[]> deleted;
bool writable = false;
bool mounted = false;
bool opened = false;

struct Key {
  uint32_t time;
  uint32_t id;
};

bool newer(Key left, Key right) {
  return left.time == right.time ? left.id > right.id : left.time > right.time;
}

struct PageIndex {
  Key rows[kPageSize + 1]{};
  size_t size = 0;

  void insert(Key key) {
    size_t at = 0;
    while (at < size && newer(rows[at], key)) ++at;
    if (at > kPageSize) return;
    if (size <= kPageSize) ++size;
    for (size_t i = size - 1; i > at; --i) rows[i] = rows[i - 1];
    rows[at] = key;
  }
};

PageIndex firstPage;
bool firstPageReady = false;

class Lock {
 public:
  Lock() { xSemaphoreTake(access, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(access); }
};

uint8_t deletedMask(uint32_t id) { return 1u << (id % 8); }

bool isDeleted(uint32_t id) { return (deleted[id / 8] & deletedMask(id)) != 0; }

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

bool loadDeleted(size_t capacity) {
  deleted.reset(new (std::nothrow) uint8_t[capacity]{});
  if (!deleted) return false;
  const int file = open(kDeletedPath, O_RDONLY);
  if (file < 0) return errno == ENOENT;
  struct stat info {};
  const bool loaded = fstat(file, &info) == 0 && info.st_size >= 0 &&
      static_cast<size_t>(info.st_size) <= capacity && read(file, deleted.get(), info.st_size) == info.st_size;
  close(file);
  if (!loaded) return false;
  for (uint32_t id = 1; id <= legacyCount + count; ++id) {
    if (isDeleted(id)) ++deletedCount;
  }
  return true;
}

const scale::Record &recordOf(const scale::Record &record) { return record; }
const scale::Record &recordOf(const SavedMeasurement &entry) { return entry.record; }

enum class ScanResult { Complete, Found, Failed };

template <typename Entry, typename Match>
ScanResult scanFile(int file, uint32_t length, uint32_t firstId, const Match &match) {
  if (!length) return ScanResult::Complete;
  if (file < 0 || lseek(file, 0, SEEK_SET) != 0) return ScanResult::Failed;
  constexpr uint32_t batchSize = 8;
  Entry batch[batchSize];
  for (uint32_t index = 0; index < length;) {
    const uint32_t size = std::min(batchSize, length - index);
    const size_t bytes = size * sizeof(Entry);
    if (read(file, batch, bytes) != static_cast<ssize_t>(bytes)) return ScanResult::Failed;
    for (uint32_t i = 0; i < size; ++i) {
      if (verified(batch[i]) && match(firstId + index + i, recordOf(batch[i]))) return ScanResult::Found;
    }
    index += size;
  }
  return ScanResult::Complete;
}

class MeasurementFiles {
 public:
  ~MeasurementFiles() {
    if (legacy_ >= 0) close(legacy_);
    if (current_ >= 0) close(current_);
  }

  bool read(uint32_t id, scale::Record &record, scale::BodyComposition &body) const {
    body = {};
    if (!id || id > legacyCount + count) return false;
    if (id <= legacyCount) return readEntry(legacy_, id - 1, record);
    SavedMeasurement entry;
    if (!readEntry(current_, id - legacyCount - 1, entry)) return false;
    record = entry.record;
    body = entry.body;
    return true;
  }

  template <typename Match>
  ScanResult scan(const Match &match) const {
    const auto currentResult = scanFile<SavedMeasurement>(current_, count, legacyCount + 1, match);
    if (currentResult != ScanResult::Complete) return currentResult;
    return scanFile<scale::Record>(legacy_, legacyCount, 1, match);
  }

 private:
  int legacy_ = legacyCount ? open(kLegacyPath, O_RDONLY) : -1;
  int current_ = count ? open(kPath, O_RDONLY) : -1;
};

bool selectPage(const MeasurementFiles &files, Key before, PageIndex &page) {
  page.size = 0;
  return files.scan([&](uint32_t id, const scale::Record &record) {
    const Key key{record.time, id};
    if (!isDeleted(id) && (!before.id || newer(before, key))) page.insert(key);
    return false;
  }) == ScanResult::Complete;
}

bool loadFirstPage(const MeasurementFiles &files) {
  if (!firstPageReady) firstPageReady = selectPage(files, {}, firstPage);
  return firstPageReady;
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
  if (record.source == scale::RecordSource::ScaleHistory) json += F(",\"source\":\"scale\"");
  if (!body && record.heightCm) json += F(",\"legacy\":true");
  if (body && body->method != scale::CompositionMethod::None) addComposition(json, *body);
  json += '}';
}
}  // namespace

bool begin() {
  access = xSemaphoreCreateMutex();
  if (!access) return false;
  if (!LittleFS.begin() && (!emptyPartition() || !LittleFS.begin(true))) return false;
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
  const size_t maxRecords = LittleFS.totalBytes() / sizeof(scale::Record);
  if (legacyCount + count > maxRecords || !loadDeleted(maxRecords / 8 + 1)) return false;
  mounted = true;
  opened = legacyReady && currentReady;
  writable = opened;
  return writable;
}

AppendResult append(const scale::Record &record, const scale::Profile &profile) {
  if (!opened || !valid(record)) return AppendResult::Failed;
  SavedMeasurement entry{record, scale::estimateBodyComposition(record, profile)};
  entry.record.checksum = checksum(entry);
  Lock lock;
  if (record.time) {
    const MeasurementFiles files;
    // Deleted rows still participate in matching scale replays.
    const auto duplicate = files.scan([&](uint32_t, const scale::Record &saved) {
      return scale::sameMeasurement(record, saved);
    });
    if (duplicate == ScanResult::Found) return AppendResult::Duplicate;
    if (duplicate == ScanResult::Failed) {
      writable = false;
      return AppendResult::Failed;
    }
  }
  const int file = open(kPath, O_RDWR | O_CREAT, 0600);
  const off_t offset = count * sizeof(entry);
  bool saved = file >= 0 && lseek(file, offset, SEEK_SET) == offset &&
               write(file, &entry, sizeof(entry)) == sizeof(entry) && fsync(file) == 0;
  if (file >= 0 && close(file) != 0) saved = false;
  if (saved) {
    ++count;
    firstPageReady = false;
  }
  writable = saved;
  return saved ? AppendResult::Saved : AppendResult::Failed;
}

DeleteResult hide(uint32_t id) {
  if (!opened) return DeleteResult::Failed;
  Lock lock;
  if (!id || id > legacyCount + count) return DeleteResult::NotFound;
  if (isDeleted(id)) return DeleteResult::Deleted;

  const int file = open(kDeletedPath, O_RDWR | O_CREAT, 0600);
  const off_t offset = id / 8;
  const uint8_t value = deleted[offset] | deletedMask(id);
  bool saved = file >= 0 && lseek(file, offset, SEEK_SET) == offset &&
               write(file, &value, 1) == 1 && fsync(file) == 0;
  if (file >= 0 && close(file) != 0) saved = false;
  if (!saved) return DeleteResult::Failed;

  deleted[offset] = value;
  ++deletedCount;
  firstPageReady = false;
  return DeleteResult::Deleted;
}

bool latest(scale::Record &record, scale::BodyComposition &body) {
  if (!mounted) return false;
  Lock lock;
  const MeasurementFiles files;
  return loadFirstPage(files) && firstPage.size > 0 && files.read(firstPage.rows[0].id, record, body);
}

String page(uint32_t before) {
  if (!mounted) return F("{\"writable\":false,\"count\":0,\"next\":0,\"rows\":[]}");
  Lock lock;
  const MeasurementFiles files;
  PageIndex older;
  scale::Record record;
  scale::BodyComposition body;
  const PageIndex *selection = &firstPage;
  bool ready;
  if (before && files.read(before, record, body)) {
    selection = &older;
    ready = selectPage(files, {record.time, before}, older);
  } else {
    ready = loadFirstPage(files);
  }

  String json;
  json.reserve(9800);
  json = F("{\"writable\":");
  json += writable && ready ? "true" : "false";
  json += F(",\"count\":");
  json += legacyCount + count - deletedCount;
  json += F(",\"rows\":[");
  bool comma = false;
  for (size_t i = 0; ready && i < std::min(selection->size, kPageSize); ++i) {
    const uint32_t id = selection->rows[i].id;
    if (!files.read(id, record, body)) continue;
    if (comma) json += ',';
    addJson(json, id, record, id <= legacyCount ? nullptr : &body);
    comma = true;
  }
  json += F("],\"next\":");
  json += ready && selection->size > kPageSize ? selection->rows[kPageSize - 1].id : 0;
  json += '}';
  return json;
}

}  // namespace history
