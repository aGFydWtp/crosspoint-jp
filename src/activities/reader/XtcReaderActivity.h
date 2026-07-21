/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool pendingScreenshot = false;

  // Streaming buffer for 1-bit (XTG) page rendering: holds a small fixed number of rows instead
  // of the whole page, avoiding a single ~48KB contiguous malloc per page turn. Allocated once in
  // onEnter(), freed in onExit(). Reused as scratch space by the 2-bit OOM fallback.
  static constexpr uint16_t kXtgStreamRows = 16;
  uint8_t* xtgStreamBuffer_ = nullptr;
  size_t xtgStreamBufferSize_ = 0;

  void renderPage();
  // wasDarkMode: dark-mode state to restore on completion (renderer.setDarkMode(false) has
  // already been applied by renderPage() by the time this is called, so it can't be recovered
  // from renderer.isDarkMode() here).
  void renderXthPageOomFallback(bool wasDarkMode);
  void saveProgress(bool isFinished = false) const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
