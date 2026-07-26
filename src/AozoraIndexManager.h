#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

struct AozoraBookEntry {
  int32_t workId;
  char title[64];
  char author[32];
  char filename[80];
};
static_assert(sizeof(AozoraBookEntry) == 180, "AozoraBookEntry のレイアウトはディスク形式と一致する必要がある");

class AozoraIndexManager {
 public:
  static constexpr const char* AOZORA_DIR = "/Aozora";
  static constexpr const char* INDEX_PATH = "/Aozora/.aozora_index.json";
  static constexpr const char* INDEX_BIN_PATH = "/Aozora/.aozora_index.bin";
  static constexpr const char* INDEX_TMP_PATH = "/Aozora/.aozora_index.bin.tmp";

  static constexpr uint8_t STATUS_ACTIVE = 0xA5;
  static constexpr uint8_t STATUS_TOMBSTONE = 0x00;

  static constexpr uint8_t BIN_HEADER_MAGIC[4] = {'A', 'Z', 'B', 'I'};
  static constexpr uint8_t BIN_HEADER_VERSION = 0x01;
  static constexpr size_t BIN_HEADER_SIZE = 8;    // magic(4) + version(1) + reserved(3)
  static constexpr size_t BIN_RECORD_SIZE = 181;  // status(1) + entry(180)

  bool loadAndPurge();
  bool isDownloaded(int workId) const;
  bool addEntry(int workId, const char* title, const char* author, const char* filename);
  bool removeEntry(int workId);
  const std::vector<AozoraBookEntry>& entries() const { return entries_; }
  /** 著者名/workId_タイトル.epub 形式の相対パスを生成 */
  static std::string makeRelativePath(int workId, const char* title, const char* author);
  static bool ensureDirectory();
  /** 著者名サブディレクトリを含む保存先を確保 */
  static bool ensureAuthorDirectory(const char* author);

 private:
  std::vector<AozoraBookEntry> entries_;
  bool saveIndex() const;

  // バイナリ形式 I/O ヘルパ（次コミットで呼び出しを追加する）
  static bool writeHeader_(HalFile& file);
  static bool checkHeader_(HalFile& file);
  static bool appendRecord_(HalFile& file, const AozoraBookEntry& entry, uint32_t& outOffset);
  static bool markTombstone_(HalFile& file, uint32_t offset);
};
