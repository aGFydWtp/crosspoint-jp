#include "Html2XtcPairingActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "Html2XtcCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TimeSync.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* kPairingsPath = "/api/device-pairings";
constexpr int kMaxPollFailures = 5;

// Strip trailing slashes from the configured server URL before appending an API path -- same
// pattern used to build the OPDS catalog URL in Html2XtcLibraryActivity.
std::string buildApiUrl(const std::string& path) {
  std::string base = HTML2XTC_STORE.getServerUrl();
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base + path;
}

std::string authorizationHeader(const std::string& pairingSecret) { return "Pairing " + pairingSecret; }
}  // namespace

void Html2XtcPairingActivity::onEnter() {
  Activity::onEnter();
  state = CHECK_WIFI;
  requestUpdate();
  checkAndConnectWifi();
}

void Html2XtcPairingActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    beginCreating();
    return;
  }
  launchWifiSelection();
}

void Html2XtcPairingActivity::launchWifiSelection() {
  state = WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void Html2XtcPairingActivity::onWifiSelectionComplete(bool connected) {
  if (!connected) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }
  beginCreating();
}

void Html2XtcPairingActivity::beginCreating() {
  state = CREATING;
  requestUpdate(true);

  // TLS chain validation needs a plausible clock; ensure NTP has synced before the first
  // verifyTls=true request (same gate OpdsBookBrowserActivity uses for html2xtc TLS servers).
  if (!TimeSync::ensureSynced(10000)) {
    LOG_ERR("XTC", "NTP sync failed; refusing to proceed with TLS-verified pairing request");
    state = ERROR;
    errorMessage = tr(STR_ERROR_TIME_SYNC_FAILED);
    requestUpdate();
    return;
  }

  createPairing();
}

void Html2XtcPairingActivity::createPairing() {
  // No on-device API distinguishes X3 vs X4 hardware; a fixed generic name is an accepted
  // fallback for the pairing request per the integration spec.
  requestedName = "Xteink";

  const std::string url = buildApiUrl(kPairingsPath);
  const std::string body = std::string("{\"requestedName\":\"") + requestedName + "\"}";
  std::string response;
  int retryAfterSeconds = 0;

  const int code = HttpDownloader::postJson(url, body, response, "", true, &retryAfterSeconds);

  if (code == 201) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, response.c_str());
    if (err) {
      LOG_ERR("XTC", "Pairing create: JSON parse failed: %s", err.c_str());
      state = ERROR;
      errorMessage = tr(STR_XTC_PAIRING_FAILED);
      requestUpdate();
      return;
    }

    pairingId = doc["pairingId"].as<std::string>();
    pairingSecret = doc["pairingSecret"].as<std::string>();
    userCode = doc["userCode"].as<std::string>();
    verificationUri = doc["verificationUri"].as<std::string>();
    pollIntervalSeconds = doc["pollIntervalSeconds"] | 5;
    if (pollIntervalSeconds <= 0) {
      pollIntervalSeconds = 5;
    }

    if (pairingId.empty() || pairingSecret.empty() || verificationUri.empty()) {
      LOG_ERR("XTC", "Pairing create: response missing required fields");
      state = ERROR;
      errorMessage = tr(STR_XTC_PAIRING_FAILED);
      requestUpdate();
      return;
    }

    LOG_DBG("XTC", "Pairing created: id=%s pollIntervalSeconds=%d", pairingId.c_str(), pollIntervalSeconds);
    state = WAITING_APPROVAL;
    lastPollMs = millis();
    consecutiveFailures = 0;
    requestUpdate();
    return;
  }

  if (code == 429) {
    LOG_ERR("XTC", "Pairing create: rate limited (retryAfter=%d)", retryAfterSeconds);
    state = ERROR;
    errorMessage = tr(STR_XTC_RATE_LIMITED);
    requestUpdate();
    return;
  }

  LOG_ERR("XTC", "Pairing create failed: http=%d", code);
  state = ERROR;
  errorMessage = tr(STR_XTC_PAIRING_FAILED);
  requestUpdate();
}

void Html2XtcPairingActivity::pollStatus() {
  lastPollMs = millis();

  const std::string url = buildApiUrl(std::string(kPairingsPath) + "/" + pairingId);
  std::string response;
  int retryAfterSeconds = 0;

  const int code = HttpDownloader::getJson(url, response, authorizationHeader(pairingSecret), true, &retryAfterSeconds);

  if (code == 429) {
    // Rate limited: adopt the server-requested interval for the next attempt. Doesn't count as
    // a failed attempt.
    if (retryAfterSeconds > 0) {
      pollIntervalSeconds = retryAfterSeconds;
    }
    return;
  }

  if (code != 200) {
    consecutiveFailures++;
    LOG_ERR("XTC", "Pairing poll failed: http=%d (attempt %d/%d)", code, consecutiveFailures, kMaxPollFailures);
    if (consecutiveFailures >= kMaxPollFailures) {
      state = ERROR;
      errorMessage = tr(STR_XTC_PAIRING_FAILED);
      requestUpdate();
    }
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, response.c_str());
  if (err) {
    LOG_ERR("XTC", "Pairing poll: JSON parse failed: %s", err.c_str());
    consecutiveFailures++;
    if (consecutiveFailures >= kMaxPollFailures) {
      state = ERROR;
      errorMessage = tr(STR_XTC_PAIRING_FAILED);
      requestUpdate();
    }
    return;
  }
  consecutiveFailures = 0;

  const std::string status = doc["status"].as<std::string>();

  if (status == "pending") {
    // Still waiting -- no redraw needed, screen content hasn't changed.
    return;
  }

  if (status == "approved") {
    pairedDeviceId = doc["deviceId"].as<std::string>();
    pairedDeviceToken = doc["deviceToken"].as<std::string>();
    if (pairedDeviceId.empty() || pairedDeviceToken.empty()) {
      LOG_ERR("XTC", "Pairing approved but response missing deviceId/deviceToken");
      state = ERROR;
      errorMessage = tr(STR_XTC_PAIRING_FAILED);
      requestUpdate();
      return;
    }
    state = SAVING;
    requestUpdate(true);
    savePairing();
    return;
  }

  if (status == "rejected") {
    state = REJECTED;
    requestUpdate();
    return;
  }

  if (status == "expired") {
    state = EXPIRED;
    requestUpdate();
    return;
  }

  // "completed" (a flat status response with no deviceId) or any unrecognized status: treated as
  // a failure since there is no credential to recover in either case.
  LOG_ERR("XTC", "Pairing poll: unexpected status '%s'", status.c_str());
  state = ERROR;
  errorMessage = tr(STR_XTC_PAIRING_FAILED);
  requestUpdate();
}

void Html2XtcPairingActivity::savePairing() {
  HTML2XTC_STORE.setPairing(pairedDeviceId, pairedDeviceToken, requestedName);
  if (!HTML2XTC_STORE.saveToFile()) {
    LOG_ERR("XTC", "Failed to save html2xtc credentials to SD card");
  }

  // Best-effort completion notice: the pairing already succeeded locally (credentials saved
  // above), so a failure here is logged only and doesn't block the success screen.
  const std::string url = buildApiUrl(std::string(kPairingsPath) + "/" + pairingId + "/complete");
  std::string response;
  const int code = HttpDownloader::postJson(url, "{}", response, authorizationHeader(pairingSecret), true, nullptr);
  if (code != 200 && code != 201) {
    LOG_ERR("XTC", "Pairing complete notification failed: http=%d", code);
  }

  state = SUCCESS;
  requestUpdate();
}

void Html2XtcPairingActivity::finishWithResult(bool cancelled) {
  ActivityResult result;
  result.isCancelled = cancelled;
  setResult(std::move(result));
  finish();
}

void Html2XtcPairingActivity::loop() {
  if (state == WIFI_SELECTION) {
    return;
  }

  if (state == CHECK_WIFI || state == CREATING || state == SAVING) {
    // These states are driven by synchronous blocking calls from onEnter()/
    // onWifiSelectionComplete()/pollStatus(), so loop() normally never observes them; guard Back
    // defensively in case control ever returns here mid-flow.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishWithResult(true);
    }
    return;
  }

  if (state == WAITING_APPROVAL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishWithResult(true);
      return;
    }
    if (millis() - lastPollMs >= static_cast<unsigned long>(pollIntervalSeconds) * 1000) {
      pollStatus();
    }
    return;
  }

  if (state == SUCCESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finishWithResult(false);
    }
    return;
  }

  if (state == EXPIRED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      beginCreating();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishWithResult(true);
    }
    return;
  }

  // REJECTED / ERROR
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishWithResult(true);
  }
}

void Html2XtcPairingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_XTC_PAIRING_TITLE));

  const auto textHeight = renderer.getLineHeight(UI_10_FONT_ID);

  if (state == CHECK_WIFI || state == CREATING || state == SAVING) {
    const char* message = (state == CHECK_WIFI) ? tr(STR_CHECKING_WIFI) : tr(STR_LOADING);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_APPROVAL) {
    const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_XTC_SCAN_QR));

    const int qrTop = startY + textHeight + metrics.verticalSpacing;
    const int bottomTextHeight = textHeight * 2 + metrics.verticalSpacing * 2;
    const int qrHeight = pageHeight - qrTop - bottomTextHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int qrWidth = pageWidth - 40;
    const Rect qrBounds(20, qrTop, qrWidth, qrHeight);
    QrUtils::drawQrCode(renderer, qrBounds, verificationUri);

    const int codeY = qrTop + qrHeight + metrics.verticalSpacing;
    const std::string codeLine = std::string(tr(STR_XTC_CODE_LABEL)) + " " + userCode;
    renderer.drawCenteredText(UI_10_FONT_ID, codeY, codeLine.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, codeY + textHeight + 5, tr(STR_XTC_WAITING_APPROVAL));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int centerY = pageHeight / 2;
  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_XTC_PAIRING_SUCCESS), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("", tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == REJECTED) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_XTC_PAIRING_REJECTED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == EXPIRED) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_XTC_PAIRING_EXPIRED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {  // ERROR
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
