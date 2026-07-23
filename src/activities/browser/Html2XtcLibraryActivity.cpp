#include "Html2XtcLibraryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "Html2XtcCredentialStore.h"
#include "Html2XtcPairingActivity.h"
#include "MappedInputManager.h"
#include "OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void Html2XtcLibraryActivity::onEnter() {
  Activity::onEnter();

  if (HTML2XTC_STORE.isPaired()) {
    // Launching from onEnter() is safe: pushActivity() only records a pending
    // action, which ActivityManager::loop() processes right after onEnter()
    // returns (see src/activities/ActivityManager.cpp:131-134).
    launchBrowser();
    return;
  }

  LOG_DBG("XTC", "Not paired, showing instruction screen");
  requestUpdate();
}

void Html2XtcLibraryActivity::launchBrowser() {
  // Build the catalog URL, avoiding a double slash if serverUrl ends with '/'
  std::string url = HTML2XTC_STORE.getServerUrl();
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += "/opds/v1/catalog.xml";

  OpdsServer server;
  server.name = tr(STR_MY_XTC);
  server.url = std::move(url);
  server.username = HTML2XTC_STORE.getDeviceId();
  server.password = HTML2XTC_STORE.getDeviceToken();
  // html2xtc is a first-party HTTPS endpoint (unlike ad-hoc/self-signed generic OPDS servers),
  // so verify its certificate chain and hostname against the embedded default CA bundle.
  server.verifyTls = true;

  browserLaunched = true;
  startActivityForResult(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, std::move(server)),
                         [this](const ActivityResult&) {
                           // OpdsBookBrowserActivity normally exits via replaceActivity (Home or
                           // Reader), which destroys the whole stack including this wrapper, so
                           // this callback is defensive: it only runs if the browser ever pops.
                           finish();
                         });
}

void Html2XtcLibraryActivity::onPairingComplete(const ActivityResult& result) {
  pairingLaunched = false;
  if (result.isCancelled) {
    // Stay on the not-paired instruction screen.
    requestUpdate();
    return;
  }
  launchBrowser();
}

void Html2XtcLibraryActivity::loop() {
  if (browserLaunched || pairingLaunched) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    pairingLaunched = true;
    startActivityForResult(std::make_unique<Html2XtcPairingActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) { onPairingComplete(result); });
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void Html2XtcLibraryActivity::render(RenderLock&&) {
  if (browserLaunched || pairingLaunched) {
    return;
  }

  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_MY_XTC), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 45, tr(STR_XTC_NOT_PAIRED));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, tr(STR_XTC_START_PAIRING), true, EpdFontFamily::BOLD);
  // Legacy manual-provisioning path, kept as a developer-facing fallback.
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_XTC_PAIRING_HINT));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 50, tr(STR_XTC_PAIRING_PATH));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
