#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for html2xtc settings. Shows connection status, device name, and server URL
 * (all read-only -- the server URL is fixed for this build, not user-editable), plus
 * actions to re-pair (also usable as the initial pairing entry point) or disconnect.
 */
class Html2XtcSettingsActivity final : public Activity {
 public:
  explicit Html2XtcSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Html2XtcSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  void handleSelection();
};
