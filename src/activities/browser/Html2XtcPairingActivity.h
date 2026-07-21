#pragma once
#include <string>

#include "../Activity.h"

/**
 * Activity implementing the html2xtc device-pairing flow (OAuth-device-code-style):
 * connect to WiFi, create a pairing request, show a QR code + short code for the user to
 * approve from their phone, poll for approval, then persist the resulting device
 * credentials via Html2XtcCredentialStore.
 *
 * See docs on the html2xtc pairing API for the exact request/response shapes; this
 * activity never displays or logs the pairingSecret/deviceToken values.
 */
class Html2XtcPairingActivity final : public Activity {
 public:
  explicit Html2XtcPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Html2XtcPairing", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CREATING || state == WAITING_APPROVAL || state == SAVING; }

 private:
  enum State { CHECK_WIFI, WIFI_SELECTION, CREATING, WAITING_APPROVAL, SAVING, SUCCESS, REJECTED, EXPIRED, ERROR };

  State state = CHECK_WIFI;

  // Pairing session data returned by POST /api/device-pairings. pairingSecret authorizes the
  // status poll and the completion notification; it is never rendered or logged.
  std::string pairingId;
  std::string pairingSecret;
  std::string userCode;
  std::string verificationUri;
  int basePollIntervalSeconds = 5;  // Server-provided cadence from the 201 create response.
  int pollIntervalSeconds = 5;      // Effective interval for the next poll; temporarily overridden
                                    // by a 429's Retry-After, then restored on the next 200.

  // Credentials obtained once the pairing is approved (see pollStatus()).
  std::string pairedDeviceId;
  std::string pairedDeviceToken;

  std::string requestedName;
  std::string errorMessage;

  unsigned long lastPollMs = 0;
  int consecutiveFailures = 0;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void beginCreating();
  void createPairing();
  void pollStatus();
  void savePairing();
  void finishWithResult(bool cancelled);
};
