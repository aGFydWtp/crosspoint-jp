#pragma once
#include "../Activity.h"
#include "../ActivityResult.h"

/**
 * Wrapper activity for the "My XTC" home-menu entry.
 *
 * If html2xtc pairing credentials exist (see Html2XtcCredentialStore), it
 * immediately launches the OPDS browser against the html2xtc server using
 * Basic auth (username=deviceId, password=deviceToken). The browser exits by
 * replacing the activity stack (Home or Reader), which also destroys this
 * wrapper; it never returns here in normal operation.
 *
 * If not paired, it shows a "not connected" instruction screen; pressing
 * Confirm launches Html2XtcPairingActivity (QR pairing flow). On success it
 * proceeds straight to the OPDS browser; on cancel it returns to this
 * instruction screen.
 */
class Html2XtcLibraryActivity final : public Activity {
  bool browserLaunched = false;
  bool pairingLaunched = false;

 public:
  explicit Html2XtcLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Html2XtcLibrary", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void launchBrowser();
  void onPairingComplete(const ActivityResult& result);
};
