#pragma once
#include <string>

// Default html2xtc server URL (override via build flag if needed)
#ifndef HTML2XTC_SERVER_URL
#define HTML2XTC_SERVER_URL "https://xtc.hr20k.com"
#endif

class Html2XtcCredentialStore;
namespace JsonSettingsIO {
bool saveHtml2Xtc(const Html2XtcCredentialStore& store, const char* path);
bool loadHtml2Xtc(Html2XtcCredentialStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing html2xtc pairing credentials on the SD card.
 * The device token is XOR-obfuscated with the device's unique hardware MAC
 * address and base64-encoded before writing to JSON (not cryptographically
 * secure, but prevents casual reading and ties credentials to the device).
 *
 * For the current development milestone the JSON file is provisioned manually
 * (a plaintext "deviceToken" key is accepted once and re-saved obfuscated).
 * The QR pairing flow will populate this store in a later phase.
 */
class Html2XtcCredentialStore {
 private:
  static Html2XtcCredentialStore instance;
  std::string serverUrl = HTML2XTC_SERVER_URL;
  std::string deviceId;
  std::string deviceToken;
  std::string deviceName;

  // Private constructor for singleton
  Html2XtcCredentialStore() = default;

  friend bool JsonSettingsIO::saveHtml2Xtc(const Html2XtcCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadHtml2Xtc(Html2XtcCredentialStore&, const char*, bool*);

 public:
  // Delete copy constructor and assignment
  Html2XtcCredentialStore(const Html2XtcCredentialStore&) = delete;
  Html2XtcCredentialStore& operator=(const Html2XtcCredentialStore&) = delete;

  // Get singleton instance
  static Html2XtcCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Clear pairing info and remove the credentials file from the SD card
  void clear();

  // True when both deviceId and deviceToken are present
  bool isPaired() const { return !deviceId.empty() && !deviceToken.empty(); }

  const std::string& getServerUrl() const { return serverUrl; }
  const std::string& getDeviceId() const { return deviceId; }
  const std::string& getDeviceToken() const { return deviceToken; }
  const std::string& getDeviceName() const { return deviceName; }

  void setServerUrl(const std::string& url);
  void setPairing(const std::string& id, const std::string& token, const std::string& name);
};

// Helper macro to access credential store
#define HTML2XTC_STORE Html2XtcCredentialStore::getInstance()
