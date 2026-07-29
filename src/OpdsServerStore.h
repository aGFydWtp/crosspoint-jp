#pragma once
#include <string>
#include <vector>

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
  // When true, HTTPS requests to this server verify the certificate chain and hostname against
  // the embedded default CA bundle (see HttpDownloader verifyTls), and the clock is NTP-synced
  // first (an unsynced clock makes every certificate look not-yet-valid).
  //
  // The struct default is false so that servers loaded from an opds.json written before this
  // field existed keep behaving as they did -- generic OPDS servers are frequently self-signed.
  // Newly created servers are secure-by-default instead: both the on-device editor
  // (OpdsSettingsActivity) and the web API (/api/opds) start them at true, so a user entering
  // token-bearing credentials does not send them over an unverified connection by accident.
  bool verifyTls = false;
};

class OpdsServerStore;
namespace JsonSettingsIO {
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore {
 private:
  static OpdsServerStore instance;
  std::vector<OpdsServer> servers;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;

  friend bool JsonSettingsIO::saveOpds(const OpdsServerStore&, const char*);
  friend bool JsonSettingsIO::loadOpds(OpdsServerStore&, const char*, bool*);

 public:
  OpdsServerStore(const OpdsServerStore&) = delete;
  OpdsServerStore& operator=(const OpdsServerStore&) = delete;

  static OpdsServerStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const { return servers; }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }

  /**
   * Migrate from legacy single-server settings in CrossPointSettings.
   * Called once during first load if no opds.json exists.
   */
  bool migrateFromSettings();
};

#define OPDS_STORE OpdsServerStore::getInstance()
