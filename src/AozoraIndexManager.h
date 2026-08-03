#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class HalFile;

struct AozoraBookEntry {
  int32_t workId;
  char title[64];
  char author[32];
  char filename[80];
  char subtitle[48];  // v2 で追加。API の subtitle
  char variant[20];   // v2 で追加。API の variant（文字遣い）。UTF-8 で最大 15 バイト + NUL
};
static_assert(sizeof(AozoraBookEntry) == 248, "AozoraBookEntry のレイアウトはディスク形式と一致する必要がある");

/**
 * v1 形式のレコード。v1 → v2 マイグレーションで旧レコードを読むためだけに使う。
 * 新規書き込みには使わない。
 */
struct AozoraBookEntryV1 {
  int32_t workId;
  char title[64];
  char author[32];
  char filename[80];
};
static_assert(sizeof(AozoraBookEntryV1) == 180, "v1 レコードのレイアウトは既存の実機データと一致する必要がある");

/**
 * 青空文庫のダウンロード履歴を管理する。
 *
 * SD 上の /Aozora/.aozora_index.bin に append-only の固定長バイナリレコードで保存し、
 * メモリ上には workId のソート済み配列のみを保持する。詳細（title/author）は
 * 表示時にファイルからページ単位で読み出す。
 *
 * これにより 500 件以上の履歴でもヒープ断片化によるクラッシュを回避する。
 */
class AozoraIndexManager {
 public:
  static constexpr const char* AOZORA_DIR = "/Aozora";
  static constexpr const char* INDEX_PATH = "/Aozora/.aozora_index.json";  // 旧形式（マイグレ用）
  static constexpr const char* INDEX_BIN_PATH = "/Aozora/.aozora_index.bin";
  static constexpr const char* INDEX_TMP_PATH = "/Aozora/.aozora_index.bin.tmp";

  static constexpr uint8_t STATUS_ACTIVE = 0xA5;
  static constexpr uint8_t STATUS_TOMBSTONE = 0x00;

  static constexpr uint8_t BIN_HEADER_MAGIC[4] = {'A', 'Z', 'B', 'I'};
  static constexpr uint8_t BIN_HEADER_VERSION = 0x02;
  static constexpr size_t BIN_HEADER_SIZE = 8;    // magic(4) + version(1) + reserved(3)
  static constexpr size_t BIN_RECORD_SIZE = 249;  // status(1) + entry(248)

  // v1 形式（subtitle/variant なし）。起動時に検出したら v2 へマイグレートする。
  static constexpr uint8_t BIN_HEADER_VERSION_V1 = 0x01;
  static constexpr size_t BIN_V1_RECORD_SIZE = 181;  // status(1) + entryV1(180)

  bool loadAndPurge();

  /** O(log N) の workId 検索 */
  bool isDownloaded(int32_t workId) const;

  /**
   * append-only 書き込み + ソート済み配列への挿入。
   * subtitle / variant は API から取得できなかった場合 nullptr / 空文字を渡してよい。
   */
  bool addEntry(int32_t workId, const char* title, const char* author, const char* filename, const char* subtitle,
                const char* variant);

  /** tombstone マークで無効化 + SD 上の EPUB ファイルを削除 */
  bool removeEntry(int32_t workId);

  /** アクティブなエントリの件数（tombstone を除く） */
  size_t activeCount() const { return activeOffsets_.size(); }

  /**
   * アクティブなエントリを挿入順で読み出す。indexInActive は [0, activeCount()) の範囲。
   * 呼び出し側は out に対する所有権を持つ（内部でヒープを保持しない）。
   */
  bool readEntryAt(size_t indexInActive, AozoraBookEntry& out) const;

  /**
   * 著者名/workId_タイトル.epub 形式の相対パスを out に書き込む。
   * 成功時 true。ホットパスから std::string を排除するため out バッファを受け取る形式。
   */
  static bool makeRelativePath(int workId, const char* title, const char* author, char* out, size_t outSize);
  static bool ensureDirectory();
  /** 著者名サブディレクトリを含む保存先を確保 */
  static bool ensureAuthorDirectory(const char* author);

 private:
  std::vector<int32_t> downloadedIds_;   // ソート済み workId
  std::vector<uint32_t> activeOffsets_;  // 挿入順で bin ファイル内のバイトオフセット

  bool loadFromBin_();
  bool migrateFromJson_();
  bool migrateBinV1ToV2_();
  bool rebuildFromDirectoryScan_();

  /**
   * bin ヘッダのフォーマットバージョンを返す。
   * BIN_VERSION_READ_FAILED (I/O 失敗) と BIN_VERSION_BAD_MAGIC (内容が壊れている) を
   * 区別するのは、前者で bin を消すと SD の一時的な不調で正常な履歴を失うため。
   */
  static int readBinVersion_();
  static constexpr int BIN_VERSION_READ_FAILED = -1;
  static constexpr int BIN_VERSION_BAD_MAGIC = -2;

  // バイナリ形式 I/O ヘルパ
  static bool writeHeader_(HalFile& file);
  static bool checkHeader_(HalFile& file);
  static bool appendRecord_(HalFile& file, const AozoraBookEntry& entry, uint32_t& outOffset);
  static bool markTombstone_(HalFile& file, uint32_t offset);
};
