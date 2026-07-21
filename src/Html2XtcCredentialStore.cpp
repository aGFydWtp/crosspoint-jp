#include "Html2XtcCredentialStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

// Initialize the static instance
Html2XtcCredentialStore Html2XtcCredentialStore::instance;

namespace {
constexpr char HTML2XTC_FILE_JSON[] = "/.crosspoint/html2xtc.json";
}  // namespace

bool Html2XtcCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveHtml2Xtc(*this, HTML2XTC_FILE_JSON);
}

bool Html2XtcCredentialStore::loadFromFile() {
  if (!Storage.exists(HTML2XTC_FILE_JSON)) {
    LOG_DBG("XTC", "No html2xtc credentials file found");
    return false;
  }

  String json = Storage.readFile(HTML2XTC_FILE_JSON);
  if (json.isEmpty()) {
    LOG_DBG("XTC", "html2xtc credentials file is empty");
    return false;
  }

  bool resave = false;
  bool result = JsonSettingsIO::loadHtml2Xtc(*this, json.c_str(), &resave);
  if (result && resave) {
    // Re-save so a manually provisioned plaintext token is persisted obfuscated
    saveToFile();
    LOG_DBG("XTC", "Resaved html2xtc credentials to update format");
  }
  return result;
}

void Html2XtcCredentialStore::clear() {
  serverUrl = HTML2XTC_SERVER_URL;
  deviceId.clear();
  deviceToken.clear();
  deviceName.clear();
  if (Storage.exists(HTML2XTC_FILE_JSON)) {
    Storage.remove(HTML2XTC_FILE_JSON);
  }
  LOG_DBG("XTC", "Cleared html2xtc credentials");
}

void Html2XtcCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url.empty() ? HTML2XTC_SERVER_URL : url;
  LOG_DBG("XTC", "Set server URL: %s", serverUrl.c_str());
}

void Html2XtcCredentialStore::setPairing(const std::string& id, const std::string& token, const std::string& name) {
  deviceId = id;
  deviceToken = token;
  deviceName = name;
  // Never log the token value; log presence/length only
  LOG_DBG("XTC", "Set pairing for device id: %s (token len=%u)", deviceId.c_str(),
          static_cast<unsigned>(deviceToken.size()));
}
