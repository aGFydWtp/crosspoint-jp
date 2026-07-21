#include "Html2XtcSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "Html2XtcCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/browser/Html2XtcPairingActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_XTC_CONNECTION_STATUS, StrId::STR_XTC_DEVICE_NAME,
                                     StrId::STR_XTC_SERVER_URL, StrId::STR_XTC_REPAIR, StrId::STR_XTC_DISCONNECT};
}  // namespace

void Html2XtcSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void Html2XtcSettingsActivity::onExit() { Activity::onExit(); }

void Html2XtcSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void Html2XtcSettingsActivity::handleSelection() {
  if (selectedIndex == 3) {
    // Re-pair. There is no device-initiated revoke API on the html2xtc server (see
    // Disconnect below), so this is also the only way to pair for the first time from
    // this screen. On success we just refresh the displayed values -- we deliberately do
    // NOT auto-navigate into the library browser here.
    startActivityForResult(std::make_unique<Html2XtcPairingActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (selectedIndex == 4) {
    // Disconnect. html2xtc has no device-initiated revoke endpoint (DELETE /api/devices/:id
    // requires the WebUI's cookie session + CSRF, which this firmware never has), so
    // "disconnecting" only clears the locally stored pairing; downloaded files are untouched.
    if (!HTML2XTC_STORE.isPaired()) {
      // Nothing to disconnect.
      return;
    }
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_XTC_DISCONNECT_CONFIRM_TITLE),
                                               tr(STR_XTC_DISCONNECT_CONFIRM_BODY)),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            HTML2XTC_STORE.clear();
          }
          requestUpdate();
        });
  }
  // Status/device name/server URL (indices 0-2) are display-only; selecting them is a no-op.
}

void Html2XtcSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_XTC_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [](int index) {
        // Draw status for each setting
        if (index == 0) {
          return HTML2XTC_STORE.isPaired() ? std::string(tr(STR_XTC_CONNECTED)) : std::string(tr(STR_XTC_DISCONNECTED));
        } else if (index == 1) {
          const auto& name = HTML2XTC_STORE.getDeviceName();
          return name.empty() ? std::string("-") : name;
        } else if (index == 2) {
          return HTML2XTC_STORE.getServerUrl();
        }
        return std::string();
      },
      true);

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
