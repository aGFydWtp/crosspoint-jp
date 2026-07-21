#pragma once
#include "../Activity.h"

/**
 * Wrapper activity for the "My XTC" home-menu entry.
 *
 * If html2xtc pairing credentials exist (see Html2XtcCredentialStore), it
 * immediately launches the OPDS browser against the html2xtc server using
 * Basic auth (username=deviceId, password=deviceToken). The browser exits by
 * replacing the activity stack (Home or Reader), which also destroys this
 * wrapper; it never returns here in normal operation.
 *
 * If not paired, it shows a "not connected" instruction screen with a Back
 * button. The QR pairing flow will be added in a later phase.
 */
class Html2XtcLibraryActivity final : public Activity {
  bool browserLaunched = false;

 public:
  explicit Html2XtcLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Html2XtcLibrary", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
