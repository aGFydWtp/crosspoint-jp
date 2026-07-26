#include "AozoraIndexManager.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

constexpr uint8_t AozoraIndexManager::BIN_HEADER_MAGIC[4];

bool AozoraIndexManager::loadAndPurge() {
  downloadedIds_.clear();
  activeOffsets_.clear();

  // 前回のマイグレが中断した場合の残骸を削除
  if (Storage.exists(INDEX_TMP_PATH)) {
    LOG_DBG("AOZORA", "Removing stale tmp file");
    Storage.remove(INDEX_TMP_PATH);
  }

  if (Storage.exists(INDEX_BIN_PATH)) {
    return loadFromBin_();
  }

  if (Storage.exists(INDEX_PATH)) {
    LOG_DBG("AOZORA", "Migrating legacy JSON to binary format");
    if (migrateFromJson_()) {
      return loadFromBin_();
    }
    LOG_ERR("AOZORA", "Migration failed, keeping legacy JSON");
    return true;
  }

  // 最後の砦: SD 上の EPUB ファイル配置から履歴を再構築
  if (Storage.exists(AOZORA_DIR)) {
    LOG_DBG("AOZORA", "Rebuilding index from SD directory scan");
    if (rebuildFromDirectoryScan_()) {
      return loadFromBin_();
    }
    LOG_ERR("AOZORA", "Directory scan rebuild failed");
  }

  LOG_DBG("AOZORA", "No index found, starting fresh");
  return true;
}

bool AozoraIndexManager::isDownloaded(int32_t workId) const {
  return std::binary_search(downloadedIds_.begin(), downloadedIds_.end(), workId);
}

bool AozoraIndexManager::addEntry(int32_t workId, const char* title, const char* author, const char* filename) {
  if (isDownloaded(workId)) return true;

  AozoraBookEntry entry;
  memset(&entry, 0, sizeof(entry));
  entry.workId = workId;
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  snprintf(entry.author, sizeof(entry.author), "%s", author);
  snprintf(entry.filename, sizeof(entry.filename), "%s", filename);

  // O_TRUNC 付きで開くと既存レコードが全消失するため、O_RDWR | O_CREAT で開く
  const bool binExists = Storage.exists(INDEX_BIN_PATH);
  HalFile file = Storage.open(INDEX_BIN_PATH, O_RDWR | O_CREAT);
  if (!file) {
    LOG_ERR("AOZORA", "addEntry: open bin for RW failed");
    return false;
  }

  if (!binExists) {
    if (!writeHeader_(file)) {
      file.close();
      Storage.remove(INDEX_BIN_PATH);
      return false;
    }
  }

  uint32_t offset = 0;
  if (!appendRecord_(file, entry, offset)) {
    file.close();
    return false;
  }
  file.close();

  auto it = std::lower_bound(downloadedIds_.begin(), downloadedIds_.end(), workId);
  downloadedIds_.insert(it, workId);
  activeOffsets_.push_back(offset);
  return true;
}

bool AozoraIndexManager::removeEntry(int32_t workId) {
  auto idIt = std::lower_bound(downloadedIds_.begin(), downloadedIds_.end(), workId);
  if (idIt == downloadedIds_.end() || *idIt != workId) return false;

  // activeOffsets_ から該当エントリを見つけて、bin ファイルの実ファイルパスを取得し
  // tombstone を書き込む。実ファイル削除もこの過程で行う。
  // O_TRUNC 付きで開くと bin 全体が消えるので O_RDWR で開く。
  HalFile file = Storage.open(INDEX_BIN_PATH, O_RDWR);
  if (!file) {
    LOG_ERR("AOZORA", "removeEntry: open bin failed");
    return false;
  }

  bool found = false;
  size_t foundIndex = 0;
  for (size_t i = 0; i < activeOffsets_.size(); ++i) {
    const uint32_t offset = activeOffsets_[i];
    if (!file.seekSet(offset + 1)) continue;  // status バイトを飛ばして workId を読む
    int32_t recWorkId = 0;
    if (file.read(&recWorkId, sizeof(recWorkId)) != static_cast<int>(sizeof(recWorkId))) continue;
    if (recWorkId != workId) continue;

    // filename を読み出して SD から削除
    AozoraBookEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (!file.seekSet(offset + 1)) {
      file.close();
      return false;
    }
    if (file.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) {
      file.close();
      return false;
    }

    if (!markTombstone_(file, offset)) {
      file.close();
      return false;
    }

    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);
    Storage.remove(fullPath);

    found = true;
    foundIndex = i;
    break;
  }
  file.close();

  if (!found) {
    LOG_ERR("AOZORA", "removeEntry: workId %d not found in bin", workId);
    return false;
  }

  downloadedIds_.erase(idIt);
  activeOffsets_.erase(activeOffsets_.begin() + foundIndex);
  return true;
}

bool AozoraIndexManager::readEntryAt(size_t indexInActive, AozoraBookEntry& out) const {
  if (indexInActive >= activeOffsets_.size()) return false;
  const uint32_t offset = activeOffsets_[indexInActive];

  HalFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_BIN_PATH, file)) {
    LOG_ERR("AOZORA", "readEntryAt: open bin failed");
    return false;
  }
  if (!file.seekSet(offset + 1)) {  // status バイトを飛ばす
    file.close();
    return false;
  }
  const int bytes = file.read(&out, sizeof(out));
  file.close();
  if (bytes != static_cast<int>(sizeof(out))) {
    LOG_ERR("AOZORA", "readEntryAt: short read at offset %u", offset);
    return false;
  }
  return true;
}

bool AozoraIndexManager::loadFromBin_() {
  HalFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_BIN_PATH, file)) {
    LOG_ERR("AOZORA", "loadFromBin: open failed");
    return false;
  }

  if (!checkHeader_(file)) {
    LOG_ERR("AOZORA", "loadFromBin: header invalid");
    file.close();
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < BIN_HEADER_SIZE) {
    file.close();
    return true;  // 空のインデックス
  }

  const size_t estimatedCount = (fileSize - BIN_HEADER_SIZE) / BIN_RECORD_SIZE;
  downloadedIds_.reserve(estimatedCount);
  activeOffsets_.reserve(estimatedCount);

  // 全レコードを走査。tombstone はスキップ、実ファイル欠損は tombstone マーク。
  size_t offset = BIN_HEADER_SIZE;
  while (offset + BIN_RECORD_SIZE <= fileSize) {
    if (!file.seekSet(offset)) break;

    uint8_t status = 0;
    if (file.read(&status, 1) != 1) break;

    if (status != STATUS_ACTIVE) {
      offset += BIN_RECORD_SIZE;
      continue;
    }

    AozoraBookEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (file.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) break;

    // 実ファイル存在チェック
    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);
    if (Storage.exists(fullPath)) {
      auto it = std::lower_bound(downloadedIds_.begin(), downloadedIds_.end(), entry.workId);
      downloadedIds_.insert(it, entry.workId);
      activeOffsets_.push_back(static_cast<uint32_t>(offset));
    } else {
      LOG_DBG("AOZORA", "Purging missing: %s", entry.filename);
      markTombstone_(file, static_cast<uint32_t>(offset));
    }

    offset += BIN_RECORD_SIZE;
  }

  file.close();
  LOG_DBG("AOZORA", "Loaded %zu active entries from bin", activeOffsets_.size());
  return true;
}

/**
 * 旧 JSON 形式 (/Aozora/.aozora_index.json) をバイナリ形式にワンショット変換する。
 *
 * 全件を JsonDocument に一度に載せると断片化を助長するので、配列を 1 要素ずつ小さな
 * JsonDocument でパースし逐次 bin に append する。.tmp に書き終えたら rename で
 * atomic swap し、成功時は旧 JSON を削除する。
 */
bool AozoraIndexManager::migrateFromJson_() {
  HalFile src;
  if (!Storage.openFileForRead("AOZORA", INDEX_PATH, src)) {
    LOG_ERR("AOZORA", "migrate: open JSON failed");
    return false;
  }

  // 先頭の '[' までスキップ
  int c = -1;
  while ((c = src.read()) != -1 && c != '[') {
  }
  if (c != '[') {
    LOG_ERR("AOZORA", "migrate: no array start");
    src.close();
    return false;
  }

  // .tmp を新規作成しヘッダを書き込む
  Storage.remove(INDEX_TMP_PATH);  // 万が一残骸があれば削除
  HalFile dst;
  if (!Storage.openFileForWrite("AOZORA", INDEX_TMP_PATH, dst)) {
    LOG_ERR("AOZORA", "migrate: open tmp for write failed");
    src.close();
    return false;
  }
  if (!writeHeader_(dst)) {
    dst.close();
    Storage.remove(INDEX_TMP_PATH);
    src.close();
    return false;
  }

  auto peekChar = [&src]() -> int {
    const size_t pos = src.position();
    int ch = src.read();
    if (ch != -1) src.seekSet(pos);
    return ch;
  };

  int migratedCount = 0;
  bool ok = true;

  while (src.available()) {
    // 空白と ',' をスキップ、']' が来たら終了
    while (src.available()) {
      const int ch = peekChar();
      if (ch == -1) break;
      if (ch == ']') {
        src.read();
        goto done;  // 配列終了
      }
      if (ch == ',' || ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') {
        src.read();
        continue;
      }
      break;
    }
    if (!src.available()) break;

    // 1 オブジェクトずつ小さな JsonDocument でパース
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, src);
    if (err) {
      LOG_ERR("AOZORA", "migrate: parse error: %s", err.c_str());
      ok = false;
      break;
    }

    JsonObject obj = doc.as<JsonObject>();
    AozoraBookEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.workId = obj["work_id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (const char*)(obj["author"] | ""));
    snprintf(entry.filename, sizeof(entry.filename), "%s", (const char*)(obj["filename"] | ""));

    // 実ファイル存在チェック（欠損は自動的にスキップ）
    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);
    if (!Storage.exists(fullPath)) {
      LOG_DBG("AOZORA", "migrate: skip missing %s", entry.filename);
      continue;
    }

    uint32_t offset = 0;
    if (!appendRecord_(dst, entry, offset)) {
      LOG_ERR("AOZORA", "migrate: appendRecord failed");
      ok = false;
      break;
    }
    migratedCount++;
  }

done:
  dst.close();
  src.close();

  if (!ok) {
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  // atomic swap: tmp を本番パスに rename
  if (!Storage.rename(INDEX_TMP_PATH, INDEX_BIN_PATH)) {
    LOG_ERR("AOZORA", "migrate: rename failed");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  // 旧 JSON を削除（rename 成功後）
  Storage.remove(INDEX_PATH);
  LOG_DBG("AOZORA", "Migrated %d entries from JSON", migratedCount);
  return true;
}

/**
 * JSON も bin も無いか壊れている場合の最終手段として、SD 上の
 * /Aozora/著者名/workId_タイトル.epub 配置からレコードを再構築する。
 *
 * makeRelativePath() の命名規則を逆手にとってファイル名から workId とタイトルを
 * 抽出する。この処理により、SD カード上に EPUB が残っていれば履歴 JSON が消えても
 * 完全に回復できる。
 */
bool AozoraIndexManager::rebuildFromDirectoryScan_() {
  HalFile rootDir;
  if (!Storage.openFileForRead("AOZORA", AOZORA_DIR, rootDir)) {
    LOG_ERR("AOZORA", "rebuild: open /Aozora failed");
    return false;
  }
  if (!rootDir.isDirectory()) {
    LOG_ERR("AOZORA", "rebuild: /Aozora is not a directory");
    rootDir.close();
    return false;
  }

  Storage.remove(INDEX_TMP_PATH);
  HalFile dst;
  if (!Storage.openFileForWrite("AOZORA", INDEX_TMP_PATH, dst)) {
    LOG_ERR("AOZORA", "rebuild: open tmp failed");
    rootDir.close();
    return false;
  }
  if (!writeHeader_(dst)) {
    dst.close();
    Storage.remove(INDEX_TMP_PATH);
    rootDir.close();
    return false;
  }

  int rebuiltCount = 0;
  bool ok = true;

  rootDir.rewindDirectory();
  HalFile authorDir = rootDir.openNextFile();
  while (authorDir.isOpen()) {
    if (!authorDir.isDirectory()) {
      authorDir.close();
      authorDir = rootDir.openNextFile();
      continue;
    }

    char authorName[48] = {};
    authorDir.getName(authorName, sizeof(authorName));

    // 隠しディレクトリ (例: .Trashes) はスキップ
    if (authorName[0] == '.') {
      authorDir.close();
      authorDir = rootDir.openNextFile();
      continue;
    }

    authorDir.rewindDirectory();
    HalFile bookFile = authorDir.openNextFile();
    while (bookFile.isOpen()) {
      if (bookFile.isDirectory()) {
        bookFile.close();
        bookFile = authorDir.openNextFile();
        continue;
      }

      char fname[96] = {};
      bookFile.getName(fname, sizeof(fname));
      bookFile.close();

      const size_t len = strlen(fname);
      if (len < 6 || strcmp(fname + len - 5, ".epub") != 0) {
        bookFile = authorDir.openNextFile();
        continue;
      }

      int workId = 0;
      if (sscanf(fname, "%d_", &workId) != 1 || workId <= 0) {
        bookFile = authorDir.openNextFile();
        continue;
      }

      const char* titleStart = strchr(fname, '_');
      if (!titleStart) {
        bookFile = authorDir.openNextFile();
        continue;
      }
      titleStart++;
      const size_t titleLen = len - (titleStart - fname) - 5;  // ".epub" を除く
      if (titleLen == 0) {
        bookFile = authorDir.openNextFile();
        continue;
      }

      AozoraBookEntry entry;
      memset(&entry, 0, sizeof(entry));
      entry.workId = workId;
      snprintf(entry.title, sizeof(entry.title), "%.*s", static_cast<int>(titleLen), titleStart);
      snprintf(entry.author, sizeof(entry.author), "%s", authorName);
      snprintf(entry.filename, sizeof(entry.filename), "%s/%s", authorName, fname);

      uint32_t offset = 0;
      if (!appendRecord_(dst, entry, offset)) {
        LOG_ERR("AOZORA", "rebuild: appendRecord failed");
        ok = false;
        break;
      }
      rebuiltCount++;

      bookFile = authorDir.openNextFile();
    }
    authorDir.close();
    if (!ok) break;
    authorDir = rootDir.openNextFile();
  }
  rootDir.close();
  dst.close();

  if (!ok) {
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  if (!Storage.rename(INDEX_TMP_PATH, INDEX_BIN_PATH)) {
    LOG_ERR("AOZORA", "rebuild: rename failed");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  LOG_DBG("AOZORA", "Rebuilt %d entries from directory scan", rebuiltCount);
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

bool AozoraIndexManager::makeRelativePath(int workId, const char* title, const char* author, char* out,
                                          size_t outSize) {
  if (!out || outSize == 0) return false;

  char safeAuthor[48];
  sanitizeForFat32(author, safeAuthor, sizeof(safeAuthor));

  char safeTitle[52];
  sanitizeForFat32(title, safeTitle, sizeof(safeTitle));

  const int written = snprintf(out, outSize, "%s/%d_%s.epub", safeAuthor, workId, safeTitle);
  return written > 0 && static_cast<size_t>(written) < outSize;
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
