#include "SectionBuildPerf.h"

#ifdef SECTION_BUILD_PERF

#include <HalStorage.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

namespace {
// 計測結果の追記先。カードを抜いて母艦で回収する前提。
// .crosspoint/ は既存のキャッシュディレクトリなので、書籍一覧には現れない。
constexpr char PERF_LOG_PATH[] = "/.crosspoint/index_perf.log";
}  // namespace

void logSectionBuildPerf(const int spineIndex, const uint32_t pageCount, const int fontId, const bool verticalMode) {
  const SectionBuildPerf& p = gSectionBuildPerf;

  // 1行に収める。ミリ秒へ丸めて出力する（µs のままだと桁が読みにくいだけで精度は不要）。
  char line[256];
  const int len =
      snprintf(line, sizeof(line),
               "t=%u spine=%d html=%u pages=%u font=%d vert=%d "
               "extract=%u read=%u parse=%u layout=%u adv=%u advn=%u img=%u pgser=%u heap=%u minheap=%u\n",
               millis(), spineIndex, p.htmlBytes, pageCount, fontId, verticalMode ? 1 : 0, p.extractUs / 1000,
               p.readUs / 1000, p.parseUs / 1000, p.layoutUs / 1000, p.advanceUs / 1000, p.advanceCalls,
               p.imageUs / 1000, p.serializeUs / 1000, ESP.getFreeHeap(), esp_get_minimum_free_heap_size());
  if (len <= 0) return;

  LOG_DBG("SCT", "PERF %s", line);

  // SDへ追記。章ビルド1回につき1行なので、書き込み自体の所要時間は計測値に対して無視できる。
  auto file = Storage.open(PERF_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (file) {
    file.write(line, strnlen(line, sizeof(line)));
    file.close();
  }
}

#endif
