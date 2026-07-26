#include "AozoraIndexManager.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstring>

constexpr uint8_t AozoraIndexManager::BIN_HEADER_MAGIC[4];

bool AozoraIndexManager::loadAndPurge() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_PATH, file)) {
    LOG_DBG("AOZORA", "No index file, starting fresh");
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("AOZORA", "Index parse error: %s", err.c_str());
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  bool needsSave = false;
  for (JsonObject obj : arr) {
    AozoraBookEntry entry;
    entry.workId = obj["work_id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (const char*)(obj["author"] | ""));
    snprintf(entry.filename, sizeof(entry.filename), "%s", (const char*)(obj["filename"] | ""));

    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);

    if (Storage.exists(fullPath)) {
      entries_.push_back(entry);
    } else {
      LOG_DBG("AOZORA", "Purging missing: %s", entry.filename);
      needsSave = true;
    }
  }

  if (needsSave) {
    saveIndex();
  }

  return true;
}

bool AozoraIndexManager::isDownloaded(int workId) const {
  for (const auto& e : entries_) {
    if (e.workId == workId) return true;
  }
  return false;
}

bool AozoraIndexManager::addEntry(int workId, const char* title, const char* author, const char* filename) {
  if (isDownloaded(workId)) return true;

  AozoraBookEntry entry;
  entry.workId = workId;
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  snprintf(entry.author, sizeof(entry.author), "%s", author);
  snprintf(entry.filename, sizeof(entry.filename), "%s", filename);
  entries_.push_back(entry);

  return saveIndex();
}

bool AozoraIndexManager::removeEntry(int workId) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->workId == workId) {
      char fullPath[160];
      snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, it->filename);
      Storage.remove(fullPath);
      entries_.erase(it);
      return saveIndex();
    }
  }
  return false;
}

bool AozoraIndexManager::saveIndex() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["work_id"] = e.workId;
    obj["title"] = e.title;
    obj["author"] = e.author;
    obj["filename"] = e.filename;
  }

  FsFile file;
  if (!Storage.openFileForWrite("AOZORA", INDEX_PATH, file)) {
    LOG_ERR("AOZORA", "Failed to open index for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

static void sanitizeForFat32(const char* src, char* dest, size_t destSize) {
  size_t pos = 0;
  for (size_t i = 0; src[i] && pos < destSize - 1; i++) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
      dest[pos++] = '_';
    } else {
      dest[pos++] = static_cast<char>(c);
    }
  }
  dest[pos] = '\0';
}

std::string AozoraIndexManager::makeRelativePath(int workId, const char* title, const char* author) {
  char safeAuthor[48];
  sanitizeForFat32(author, safeAuthor, sizeof(safeAuthor));

  char safeTitle[52];
  sanitizeForFat32(title, safeTitle, sizeof(safeTitle));

  char result[160];
  snprintf(result, sizeof(result), "%s/%d_%s.epub", safeAuthor, workId, safeTitle);
  return std::string(result);
}

bool AozoraIndexManager::ensureAuthorDirectory(const char* author) {
  char safeAuthor[48];
  sanitizeForFat32(author, safeAuthor, sizeof(safeAuthor));

  char dirPath[80];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", AOZORA_DIR, safeAuthor);

  if (Storage.exists(dirPath)) return true;
  return Storage.mkdir(dirPath);
}

bool AozoraIndexManager::ensureDirectory() {
  if (Storage.exists(AOZORA_DIR)) return true;
  return Storage.mkdir(AOZORA_DIR);
}

// --- バイナリ形式 I/O ヘルパ ---

bool AozoraIndexManager::writeHeader_(HalFile& file) {
  uint8_t header[BIN_HEADER_SIZE] = {
      BIN_HEADER_MAGIC[0],
      BIN_HEADER_MAGIC[1],
      BIN_HEADER_MAGIC[2],
      BIN_HEADER_MAGIC[3],
      BIN_HEADER_VERSION,
      0x00,
      0x00,
      0x00,
  };
  if (!file.seekSet(0)) {
    LOG_ERR("AOZORA", "writeHeader: seek(0) failed");
    return false;
  }
  if (file.write(header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("AOZORA", "writeHeader: write failed");
    return false;
  }
  return true;
}

bool AozoraIndexManager::checkHeader_(HalFile& file) {
  if (!file.seekSet(0)) return false;
  uint8_t header[BIN_HEADER_SIZE];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (memcmp(header, BIN_HEADER_MAGIC, sizeof(BIN_HEADER_MAGIC)) != 0) return false;
  if (header[4] != BIN_HEADER_VERSION) return false;
  return true;
}

bool AozoraIndexManager::appendRecord_(HalFile& file, const AozoraBookEntry& entry, uint32_t& outOffset) {
  const size_t endPos = file.fileSize();
  if (!file.seekSet(endPos)) {
    LOG_ERR("AOZORA", "appendRecord: seek(end) failed");
    return false;
  }
  const uint8_t status = STATUS_ACTIVE;
  if (file.write(&status, 1) != 1) {
    LOG_ERR("AOZORA", "appendRecord: write status failed");
    return false;
  }
  if (file.write(&entry, sizeof(entry)) != sizeof(entry)) {
    LOG_ERR("AOZORA", "appendRecord: write entry failed");
    return false;
  }
  outOffset = static_cast<uint32_t>(endPos);
  return true;
}

bool AozoraIndexManager::markTombstone_(HalFile& file, uint32_t offset) {
  if (!file.seekSet(offset)) {
    LOG_ERR("AOZORA", "markTombstone: seek(%u) failed", offset);
    return false;
  }
  const uint8_t status = STATUS_TOMBSTONE;
  if (file.write(&status, 1) != 1) {
    LOG_ERR("AOZORA", "markTombstone: write failed");
    return false;
  }
  file.flush();
  return true;
}
