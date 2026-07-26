// 青空文庫ダウンロード履歴のバイナリ形式契約テスト。
//
// AozoraIndexManager は SD I/O と ArduinoJson に依存するためホストで直接動かせないが、
// バイナリファイルフォーマット (Header + 181B レコード) の raw byte 契約と、
// 構造体レイアウトは HalStorage / ArduinoJson 非依存で検証できる。
//
// このテストが壊れる = ディスク上のファイルフォーマットが変わり互換性が壊れる、
// または既存の実機データが読めなくなることを意味する。契約が変わる場合は
// BIN_HEADER_VERSION をインクリメントし、互換ロジックを追加すること。

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "src/AozoraIndexManager.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                                              \
  do {                                                                                               \
    if ((a) != (b)) {                                                                                \
      fprintf(stderr, "  FAIL: %s:%d: (%s) == %ld, expected %ld\n", __FILE__, __LINE__, #a,          \
              static_cast<long>(a), static_cast<long>(b));                                           \
      testsFailed++;                                                                                 \
      return;                                                                                        \
    }                                                                                                \
  } while (0)

#define ASSERT_EQ_U(a, b)                                                                            \
  do {                                                                                               \
    if ((a) != (b)) {                                                                                \
      fprintf(stderr, "  FAIL: %s:%d: (%s) == %lu, expected %lu\n", __FILE__, __LINE__, #a,          \
              static_cast<unsigned long>(a), static_cast<unsigned long>(b));                         \
      testsFailed++;                                                                                 \
      return;                                                                                        \
    }                                                                                                \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

// ============================================================================
// 1. AozoraBookEntry 構造体レイアウトの契約
// ============================================================================

static void test_entry_layout_size() {
  ASSERT_EQ_U(sizeof(AozoraBookEntry), 180u);
  PASS();
}

static void test_entry_field_offsets() {
  // オフセットが変わるとバイナリ形式が壊れる
  ASSERT_EQ_U(offsetof(AozoraBookEntry, workId), 0u);
  ASSERT_EQ_U(offsetof(AozoraBookEntry, title), 4u);
  ASSERT_EQ_U(offsetof(AozoraBookEntry, author), 68u);
  ASSERT_EQ_U(offsetof(AozoraBookEntry, filename), 100u);
  PASS();
}

static void test_entry_field_sizes() {
  AozoraBookEntry e{};
  ASSERT_EQ_U(sizeof(e.workId), 4u);
  ASSERT_EQ_U(sizeof(e.title), 64u);
  ASSERT_EQ_U(sizeof(e.author), 32u);
  ASSERT_EQ_U(sizeof(e.filename), 80u);
  PASS();
}

// ============================================================================
// 2. Header / Record サイズ定数
// ============================================================================

static void test_header_size() {
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_SIZE, 8u);
  PASS();
}

static void test_record_size() {
  // レコードは status(1) + entry(180) = 181
  ASSERT_EQ_U(AozoraIndexManager::BIN_RECORD_SIZE, 1u + sizeof(AozoraBookEntry));
  ASSERT_EQ_U(AozoraIndexManager::BIN_RECORD_SIZE, 181u);
  PASS();
}

static void test_status_bytes() {
  // status バイトは 0xA5 / 0x00 で、初期化された領域が誤って ACTIVE と誤認されない
  ASSERT_EQ_U(AozoraIndexManager::STATUS_ACTIVE, 0xA5u);
  ASSERT_EQ_U(AozoraIndexManager::STATUS_TOMBSTONE, 0x00u);
  // 0xFF (フラッシュのデフォルト値) と 0x00 (memset ゼロ) が両方 ACTIVE と衝突しないこと
  ASSERT_TRUE(AozoraIndexManager::STATUS_ACTIVE != 0xFFu);
  ASSERT_TRUE(AozoraIndexManager::STATUS_ACTIVE != 0x00u);
  PASS();
}

static void test_header_magic() {
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_MAGIC[0], 'A');
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_MAGIC[1], 'Z');
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_MAGIC[2], 'B');
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_MAGIC[3], 'I');
  ASSERT_EQ_U(AozoraIndexManager::BIN_HEADER_VERSION, 0x01u);
  PASS();
}

// ============================================================================
// 3. パス定数
// ============================================================================

static void test_path_constants() {
  ASSERT_TRUE(strcmp(AozoraIndexManager::AOZORA_DIR, "/Aozora") == 0);
  ASSERT_TRUE(strcmp(AozoraIndexManager::INDEX_PATH, "/Aozora/.aozora_index.json") == 0);
  ASSERT_TRUE(strcmp(AozoraIndexManager::INDEX_BIN_PATH, "/Aozora/.aozora_index.bin") == 0);
  ASSERT_TRUE(strcmp(AozoraIndexManager::INDEX_TMP_PATH, "/Aozora/.aozora_index.bin.tmp") == 0);
  PASS();
}

// ============================================================================
// 4. int32_t のリトルエンディアン書き込み
// ESP32-C3 (RISC-V) はリトルエンディアン。バイナリの int32_t は memcpy で読み書きされる想定。
// ============================================================================

static void test_little_endian_workId_roundtrip() {
  AozoraBookEntry e{};
  e.workId = 0x12345678;
  snprintf(e.title, sizeof(e.title), "roundtrip");

  // struct を一度メモリバッファに書き出し、リトルエンディアンで戻せることを確認
  uint8_t buf[sizeof(AozoraBookEntry)];
  memcpy(buf, &e, sizeof(e));

  // workId のバイトオフセットは 0-3、リトルエンディアン
  ASSERT_EQ_U(buf[0], 0x78u);
  ASSERT_EQ_U(buf[1], 0x56u);
  ASSERT_EQ_U(buf[2], 0x34u);
  ASSERT_EQ_U(buf[3], 0x12u);

  // 逆方向
  AozoraBookEntry back{};
  memcpy(&back, buf, sizeof(back));
  ASSERT_EQ(back.workId, 0x12345678);
  ASSERT_TRUE(strcmp(back.title, "roundtrip") == 0);
  PASS();
}

// ============================================================================
// 5. ページキャッシュの境界計算
// AozoraActivity::loadDownloadedPage で使う selectedIndex → pageStart の算出は
// (selectedIndex_ / DL_PAGE_SIZE) * DL_PAGE_SIZE。エッジケースを検証する。
// ============================================================================

static int pageStartFor(int selectedIndex) {
  constexpr int DL_PAGE_SIZE = 30;
  if (selectedIndex < 0) return 0;
  return (selectedIndex / DL_PAGE_SIZE) * DL_PAGE_SIZE;
}

static void test_page_boundary_calculation() {
  ASSERT_EQ(pageStartFor(0), 0);
  ASSERT_EQ(pageStartFor(1), 0);
  ASSERT_EQ(pageStartFor(29), 0);
  ASSERT_EQ(pageStartFor(30), 30);
  ASSERT_EQ(pageStartFor(31), 30);
  ASSERT_EQ(pageStartFor(59), 30);
  ASSERT_EQ(pageStartFor(60), 60);
  ASSERT_EQ(pageStartFor(1000), 990);  // 1000 / 30 = 33, 33 * 30 = 990
  PASS();
}

// ============================================================================
// 6. sorted vector<int32_t> による isDownloaded の O(log N) 検索契約
// ============================================================================

static void test_sorted_workid_binary_search() {
  std::vector<int32_t> ids;
  const int32_t testIds[] = {5, 10, 15, 20, 25, 30, 100, 500, 1000, 99999};
  for (int32_t id : testIds) {
    auto it = std::lower_bound(ids.begin(), ids.end(), id);
    ids.insert(it, id);
  }
  // insert がソート順を維持している
  ASSERT_TRUE(std::is_sorted(ids.begin(), ids.end()));

  for (int32_t id : testIds) {
    ASSERT_TRUE(std::binary_search(ids.begin(), ids.end(), id));
  }
  ASSERT_TRUE(!std::binary_search(ids.begin(), ids.end(), 0));
  ASSERT_TRUE(!std::binary_search(ids.begin(), ids.end(), 6));
  ASSERT_TRUE(!std::binary_search(ids.begin(), ids.end(), 1000000));

  // 挿入順 (100 が末尾に来る) でも sorted 順を維持
  {
    std::vector<int32_t> ids2;
    const int32_t inputOrder[] = {50, 20, 100, 10, 30, 80};
    for (int32_t id : inputOrder) {
      auto it = std::lower_bound(ids2.begin(), ids2.end(), id);
      ids2.insert(it, id);
    }
    ASSERT_TRUE(std::is_sorted(ids2.begin(), ids2.end()));
    ASSERT_EQ(ids2[0], 10);
    ASSERT_EQ(ids2[1], 20);
    ASSERT_EQ(ids2[2], 30);
    ASSERT_EQ(ids2[3], 50);
    ASSERT_EQ(ids2[4], 80);
    ASSERT_EQ(ids2[5], 100);
  }
  PASS();
}

// ============================================================================
// 7. バイナリファイルの実際のバイト列を組み立ててオフセット整合を確認
// ============================================================================

static void test_bin_layout_construction() {
  std::vector<uint8_t> file;

  // Header (8B): magic "AZBI" + version 0x01 + reserved 0x00 0x00 0x00
  file.push_back('A');
  file.push_back('Z');
  file.push_back('B');
  file.push_back('I');
  file.push_back(0x01);
  file.push_back(0x00);
  file.push_back(0x00);
  file.push_back(0x00);
  ASSERT_EQ_U(file.size(), AozoraIndexManager::BIN_HEADER_SIZE);

  // 1 レコード目 (181B): status 0xA5 + entry
  file.push_back(AozoraIndexManager::STATUS_ACTIVE);
  AozoraBookEntry e1{};
  e1.workId = 42;
  snprintf(e1.title, sizeof(e1.title), "sample");
  snprintf(e1.author, sizeof(e1.author), "author1");
  snprintf(e1.filename, sizeof(e1.filename), "author1/42_sample.epub");
  const uint8_t* e1bytes = reinterpret_cast<const uint8_t*>(&e1);
  file.insert(file.end(), e1bytes, e1bytes + sizeof(e1));
  ASSERT_EQ_U(file.size(), AozoraIndexManager::BIN_HEADER_SIZE + AozoraIndexManager::BIN_RECORD_SIZE);

  // 2 レコード目 = tombstone (status 0x00 + 何らかの entry)
  file.push_back(AozoraIndexManager::STATUS_TOMBSTONE);
  AozoraBookEntry e2{};
  e2.workId = 99;
  const uint8_t* e2bytes = reinterpret_cast<const uint8_t*>(&e2);
  file.insert(file.end(), e2bytes, e2bytes + sizeof(e2));

  // 3 レコード目 = active
  file.push_back(AozoraIndexManager::STATUS_ACTIVE);
  AozoraBookEntry e3{};
  e3.workId = 200;
  snprintf(e3.title, sizeof(e3.title), "third");
  snprintf(e3.author, sizeof(e3.author), "authorN");
  snprintf(e3.filename, sizeof(e3.filename), "authorN/200_third.epub");
  const uint8_t* e3bytes = reinterpret_cast<const uint8_t*>(&e3);
  file.insert(file.end(), e3bytes, e3bytes + sizeof(e3));

  // 各レコードのオフセットが式通り (HEADER + i * RECORD_SIZE) に配置されている
  const size_t rec1Offset = AozoraIndexManager::BIN_HEADER_SIZE;
  const size_t rec2Offset = AozoraIndexManager::BIN_HEADER_SIZE + AozoraIndexManager::BIN_RECORD_SIZE;
  const size_t rec3Offset = AozoraIndexManager::BIN_HEADER_SIZE + 2 * AozoraIndexManager::BIN_RECORD_SIZE;

  ASSERT_EQ_U(file[rec1Offset], AozoraIndexManager::STATUS_ACTIVE);
  ASSERT_EQ_U(file[rec2Offset], AozoraIndexManager::STATUS_TOMBSTONE);
  ASSERT_EQ_U(file[rec3Offset], AozoraIndexManager::STATUS_ACTIVE);

  // Record 1 の workId (offset + 1、リトルエンディアン)
  ASSERT_EQ_U(file[rec1Offset + 1], 42u);
  ASSERT_EQ_U(file[rec1Offset + 2], 0u);
  ASSERT_EQ_U(file[rec1Offset + 3], 0u);
  ASSERT_EQ_U(file[rec1Offset + 4], 0u);

  // Record 3 の workId (200 = 0xC8)
  ASSERT_EQ_U(file[rec3Offset + 1], 0xC8u);
  ASSERT_EQ_U(file[rec3Offset + 2], 0u);

  // 想定通りの loadFromBin_ の走査: HEADER から RECORD_SIZE 刻みで active のみ拾う
  std::vector<size_t> activeOffsets;
  size_t offset = AozoraIndexManager::BIN_HEADER_SIZE;
  while (offset + AozoraIndexManager::BIN_RECORD_SIZE <= file.size()) {
    if (file[offset] == AozoraIndexManager::STATUS_ACTIVE) {
      activeOffsets.push_back(offset);
    }
    offset += AozoraIndexManager::BIN_RECORD_SIZE;
  }
  ASSERT_EQ_U(activeOffsets.size(), 2u);
  ASSERT_EQ_U(activeOffsets[0], rec1Offset);
  ASSERT_EQ_U(activeOffsets[1], rec3Offset);
  PASS();
}

// ============================================================================
// 8. tombstone マーキング後、ファイルサイズは変わらないという契約
// ============================================================================

static void test_tombstone_preserves_record_positions() {
  std::vector<uint8_t> file(AozoraIndexManager::BIN_HEADER_SIZE + 3 * AozoraIndexManager::BIN_RECORD_SIZE);
  memset(file.data(), 0, file.size());

  // すべて ACTIVE にする
  file[AozoraIndexManager::BIN_HEADER_SIZE] = AozoraIndexManager::STATUS_ACTIVE;
  file[AozoraIndexManager::BIN_HEADER_SIZE + AozoraIndexManager::BIN_RECORD_SIZE] = AozoraIndexManager::STATUS_ACTIVE;
  file[AozoraIndexManager::BIN_HEADER_SIZE + 2 * AozoraIndexManager::BIN_RECORD_SIZE] =
      AozoraIndexManager::STATUS_ACTIVE;

  const size_t sizeBefore = file.size();

  // 中央のレコードを tombstone にする (実装と同じく status バイトのみ書き換え)
  file[AozoraIndexManager::BIN_HEADER_SIZE + AozoraIndexManager::BIN_RECORD_SIZE] =
      AozoraIndexManager::STATUS_TOMBSTONE;

  ASSERT_EQ_U(file.size(), sizeBefore);  // サイズ不変
  // 前後のオフセットは維持されている
  ASSERT_EQ_U(file[AozoraIndexManager::BIN_HEADER_SIZE], AozoraIndexManager::STATUS_ACTIVE);
  ASSERT_EQ_U(file[AozoraIndexManager::BIN_HEADER_SIZE + 2 * AozoraIndexManager::BIN_RECORD_SIZE],
              AozoraIndexManager::STATUS_ACTIVE);
  PASS();
}

// ============================================================================
// 9. workId=0 の扱い
// rebuildFromDirectoryScan_ で sscanf が失敗した場合 workId=0 は無効と判定する仕様
// ============================================================================

static void test_workid_zero_is_invalid_marker() {
  // sscanf(fname, "%d_", &workId) が失敗すれば workId は書き換えられず初期値 0 のまま。
  // 実装は「workId <= 0 ならスキップ」なので、0 を無効値としてマーカーに使える。
  int workId = 0;
  int matched = sscanf("garbage_name.epub", "%d_", &workId);
  ASSERT_EQ(matched, 0);
  ASSERT_EQ(workId, 0);

  workId = 0;
  matched = sscanf("42_title.epub", "%d_", &workId);
  ASSERT_EQ(matched, 1);
  ASSERT_EQ(workId, 42);
  PASS();
}

// ============================================================================
// テストランナー
// ============================================================================

typedef void (*TestFn)();

struct TestCase {
  const char* name;
  TestFn fn;
};

int main() {
  const TestCase tests[] = {
      {"entry_layout_size", test_entry_layout_size},
      {"entry_field_offsets", test_entry_field_offsets},
      {"entry_field_sizes", test_entry_field_sizes},
      {"header_size", test_header_size},
      {"record_size", test_record_size},
      {"status_bytes", test_status_bytes},
      {"header_magic", test_header_magic},
      {"path_constants", test_path_constants},
      {"little_endian_workId_roundtrip", test_little_endian_workId_roundtrip},
      {"page_boundary_calculation", test_page_boundary_calculation},
      {"sorted_workid_binary_search", test_sorted_workid_binary_search},
      {"bin_layout_construction", test_bin_layout_construction},
      {"tombstone_preserves_record_positions", test_tombstone_preserves_record_positions},
      {"workid_zero_is_invalid_marker", test_workid_zero_is_invalid_marker},
  };

  for (const auto& t : tests) {
    printf("Running %s...\n", t.name);
    t.fn();
  }

  printf("\n=== Results ===\n");
  printf("  Passed: %d\n", testsPassed);
  printf("  Failed: %d\n", testsFailed);

  return testsFailed == 0 ? 0 : 1;
}
