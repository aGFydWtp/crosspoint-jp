#pragma once

// 章ビルド（インデックス）処理の内訳を計測するためのカウンタ群。
//
// 1章構成の巨大EPUBでインデックスが極端に遅い問題（Issue #99）の原因切り分け用。
// 既存の計時ログ（ChapterHtmlSlimParser の "Time to parse and build pages"）は
// パース・レイアウト・直列化の合計しか出さないため、どこが支配的なのか判断できない。
//
// 計測点の包含関係:
//   章ビルド全体
//   ├─ extractUs   zip -> .tmp_<n>.html への展開
//   └─ parseUs     XML_ParseBuffer の累計（expatコールバック内の処理を含む）
//      ├─ readUs   一時ファイルの read 累計（parseUs とは別枠、パースループ内）
//      ├─ layoutUs   ParsedText の行分割・カラム分割（parseUs の内数）
//      │  └─ advanceUs   SDカードフォントの advance テーブル構築（layoutUs の内数）
//      ├─ imageUs    画像の抽出とデコード（parseUs の内数）
//      └─ serializeUs  section.bin へのページ直列化（parseUs の内数）
//
// 注意: imageUs は画像タグの処理区間まるごとを測るため、その中で発生する
// テキストブロックの flush（layoutUs）やページ直列化（serializeUs）と重複する。
// 内訳の合計が parseUs を超えることがあるのは正常で、支配項の特定が目的。
//
// TextBlock::render() 内の advance 構築は描画時の処理でインデックス経路ではないため、
// 意図的に計装していない（章ビルド中には呼ばれない）。
//
// LOG_DBG が有効なビルド（default 環境）でのみ実体を持ち、gh_release（LOG_LEVEL=1）や
// slim（ENABLE_SERIAL_LOG なし）ではプリプロセッサで完全に消える。計測用の
// micros() 呼び出しやカウンタ変数もリリースビルドには一切残らない。

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
#define SECTION_BUILD_PERF 1
#endif

#ifdef SECTION_BUILD_PERF

#include <Arduino.h>
#include <Logging.h>

#include <cstdint>

struct SectionBuildPerf {
  uint32_t extractUs;
  uint32_t readUs;
  uint32_t parseUs;
  uint32_t layoutUs;
  uint32_t advanceUs;
  uint32_t imageUs;
  uint32_t serializeUs;
  uint32_t advanceCalls;
  uint32_t htmlBytes;

  void reset() { *this = SectionBuildPerf{}; }
};

// C++20 の inline 変数。DBGビルドでのみ 36 バイトの DRAM を消費する。
// 章ビルドは常に同期実行（サイレントインデックスも loop() 内で直列に走る）なので、
// 単一のグローバルカウンタで競合しない。
inline SectionBuildPerf gSectionBuildPerf{};

// スコープを抜けるときに経過時間（µs）をカウンタへ加算する。
// 早期 return の多い区間（画像処理など）でも取りこぼさないよう RAII にしている。
class ScopedPerfTimer {
 public:
  explicit ScopedPerfTimer(uint32_t& target) : target(target), startUs(micros()) {}
  ~ScopedPerfTimer() { target += micros() - startUs; }
  ScopedPerfTimer(const ScopedPerfTimer&) = delete;
  ScopedPerfTimer& operator=(const ScopedPerfTimer&) = delete;

 private:
  uint32_t& target;
  const uint32_t startUs;
};

inline void logSectionBuildPerf(const int spineIndex, const uint32_t pageCount) {
  const SectionBuildPerf& p = gSectionBuildPerf;
  LOG_DBG("SCT",
          "PERF spine=%d html=%uB pages=%u | extract=%ums read=%ums parse=%ums "
          "(layout=%ums adv=%ums/n=%u img=%ums pgser=%ums) heap=%u",
          spineIndex, p.htmlBytes, pageCount, p.extractUs / 1000, p.readUs / 1000, p.parseUs / 1000, p.layoutUs / 1000,
          p.advanceUs / 1000, p.advanceCalls, p.imageUs / 1000, p.serializeUs / 1000, ESP.getFreeHeap());
}

#define SECTION_PERF_CONCAT_INNER(a, b) a##b
#define SECTION_PERF_CONCAT(a, b) SECTION_PERF_CONCAT_INNER(a, b)

// スコープ全体を計測する（早期 return があっても加算される）
#define SECTION_PERF_SCOPE(field) \
  const ScopedPerfTimer SECTION_PERF_CONCAT(perfTimer, __LINE__)(gSectionBuildPerf.field)
// 単一の式だけを計測する（const 変数の初期化を挟みたい箇所向け）
#define SECTION_PERF_BEGIN(name) const uint32_t name = micros()
#define SECTION_PERF_END(name, field) gSectionBuildPerf.field += micros() - (name)
#define SECTION_PERF_COUNT(field) (++gSectionBuildPerf.field)
#define SECTION_PERF_SET(field, value) (gSectionBuildPerf.field = (value))
#define SECTION_PERF_RESET() gSectionBuildPerf.reset()
#define SECTION_PERF_LOG(spineIndex, pageCount) logSectionBuildPerf((spineIndex), (pageCount))

#else

#define SECTION_PERF_SCOPE(field) ((void)0)
#define SECTION_PERF_BEGIN(name) ((void)0)
#define SECTION_PERF_END(name, field) ((void)0)
#define SECTION_PERF_COUNT(field) ((void)0)
#define SECTION_PERF_SET(field, value) ((void)0)
#define SECTION_PERF_RESET() ((void)0)
#define SECTION_PERF_LOG(spineIndex, pageCount) ((void)0)

#endif
