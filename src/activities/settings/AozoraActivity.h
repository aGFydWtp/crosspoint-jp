#pragma once

#include <ArduinoJson.h>

#include <deque>
#include <string>
#include <vector>

#include "AozoraIndexManager.h"
#include "FavoriteAuthorsManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AozoraActivity : public Activity {
 public:
  explicit AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    TOP_MENU,
    KANA_SELECT,
    KANA_CHAR_SELECT,
    GENRE_SELECT,
    AUTHOR_LIST,
    WORK_LIST,
    WORK_DETAIL,
    DOWNLOADING,
    DOWNLOADED_LIST,
    FAVORITE_AUTHORS,
    AUTHOR_ACTION,
    LOADING,
    ERROR,
  };

  enum SearchMode { SEARCH_AUTHOR, SEARCH_TITLE };

  struct AuthorEntry {
    int id;
    char name[48];
    char kana[48];
    int workCount;
  };

  struct WorkEntry {
    int id;
    char title[80];
    char kana[48];
    char ndc[8];
    char author[48];
    char subtitle[64];  // API の subtitle。CJK 21 文字相当
    // API の variant（文字遣い）。実測値は「新字新仮名」「新字旧仮名」「旧字旧仮名」の
    // 3 種でいずれも UTF-8 15 バイト。char[16] では NUL 込みで余裕がないため 20 とする。
    char variant[20];
  };

  State state_ = WIFI_SELECTION;
  SearchMode searchMode_ = SEARCH_AUTHOR;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  std::string errorMessage_;

  // State history stack for Back navigation
  std::vector<State> stateStack_;
  std::vector<int> selectedIndexStack_;

  // API result buffers.
  //
  // std::deque, not std::vector: a vector holds its elements in one contiguous
  // allocation, and the "ア" author row alone is 206 x 104B = 21.4KB of it. That
  // single block is what fragments the heap out of the ~16.5KB contiguous chunk
  // a TLS handshake needs, which is how the author listing ended up failing with
  // ESP_ERR_HTTP_CONNECT at heap=61KB / blk=20KB. deque spreads the same data
  // over 512-byte nodes, so it never needs a large contiguous region, and
  // operator[] keeps every call site unchanged.
  std::deque<AuthorEntry> authors_;
  std::deque<WorkEntry> works_;

  // 同名作品の識別用ビットマスク。works_ のインデックス i のビットが立っていることを表す。
  // 描画のたびに O(N^2) の strcmp を回さないよう、パース完了時に一度だけ計算する。
  //   dupTitleMask_   : 同一ページ内に同名タイトルが存在する
  //   ambiguousMask_  : 副題・文字遣いまで一致し、作品 ID でしか区別できない
  //                     （例: 芥川竜之介「仙人」id 143 / 144）
  uint32_t dupTitleMask_ = 0;
  uint32_t ambiguousMask_ = 0;

  // Works pagination
  int worksTotal_ = 0;
  int worksOffset_ = 0;
  static constexpr int WORKS_PAGE_SIZE = 30;
  static_assert(WORKS_PAGE_SIZE <= 32, "重複判定マスクが uint32_t に収まる必要がある");
  char lastWorksQuery_[64] = {};  // 再取得用にクエリを保持

  // Selected kana row index (for KANA_CHAR_SELECT)
  int selectedKanaRowIndex_ = 0;

  // 最後に使った作家検索の50音行（再取得用）
  char lastAuthorsKanaPrefix_[8] = {};

  // Selected item info (carried across states)
  int selectedAuthorId_ = 0;
  char selectedAuthorName_[48] = {};
  int selectedWorkId_ = 0;
  char selectedWorkTitle_[80] = {};
  char selectedWorkAuthor_[48] = {};
  char selectedWorkSubtitle_[64] = {};
  char selectedWorkVariant_[20] = {};
  char selectedWorkNdc_[8] = {};

  // Download progress
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  // Download index manager
  AozoraIndexManager indexManager_;
  FavoriteAuthorsManager favoritesManager_;

  // DOWNLOADED_LIST 表示用のページキャッシュ。
  // indexManager_ からオンデマンドで読み出し、常駐メモリを最小化する。
  static constexpr int DL_PAGE_SIZE = 30;
  static_assert(DL_PAGE_SIZE <= 32, "重複判定マスクが uint32_t に収まる必要がある");
  AozoraBookEntry dlPageCache_[DL_PAGE_SIZE] = {};
  int dlPageStart_ = -1;  // -1 = キャッシュ無効
  int dlPageCount_ = 0;
  // dlPageCache_ 内の重複判定マスク。ビット位置はキャッシュ内のローカルインデックス。
  uint32_t dlDupTitleMask_ = 0;
  uint32_t dlAmbiguousMask_ = 0;

  // ページキャッシュに [start, start+DL_PAGE_SIZE) 範囲のエントリを読み込む。
  void loadDownloadedPage(int start);
  // 追加・削除でキャッシュを無効化。次の描画で自動的に再ロードされる。
  void invalidateDownloadedPageCache() {
    dlPageStart_ = -1;
    dlPageCount_ = 0;
  }

  // AUTHOR_ACTION state
  int actionMenuIndex_ = 0;

  static constexpr const char* API_BASE = "https://aozora-epub-api.vercel.app";

  // State navigation
  void pushState(State newState);
  void popState();

  // WiFi
  void onWifiSelectionComplete(bool success);

  // API calls (blocking -- call from correct state)
  bool fetchAuthors(const char* kanaPrefix);
  bool fetchWorks(const char* queryParam);
  bool downloadBook();
  bool updateBook();
  /**
   * 選択中の作品をリーダーで開く。呼び出し後は this が破棄され得るため、
   * 呼び出し元は即座に return してメンバに触れないこと。
   */
  void openSelectedWorkInReader();
  // Download destPath from url with the same retry policy as the listing calls.
  // Sets errorMessage_ and returns false once every attempt has failed.
  bool downloadWithRetry(const char* url, const char* destPath);

  // JSON parsing
  bool parseAuthorsJson(JsonDocument& doc);
  bool parseWorksJson(JsonDocument& doc);

  /** works_ から dupTitleMask_ / ambiguousMask_ を再計算する */
  void computeWorkDuplicateMasks();
  /** WORK_LIST の 2 行目（著者・副題・文字遣い・作品 ID）を組み立てる */
  std::string buildWorkListSubtitle(int index) const;
  /** DOWNLOADED_LIST の 2 行目を組み立てる。index はリスト全体での絶対インデックス */
  std::string buildDownloadedListSubtitle(int index) const;
};
