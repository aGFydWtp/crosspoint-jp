#pragma once
#include <OpdsParser.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "OpdsServerStore.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  int selectorIndex = 0;
  std::string errorMessage;
  std::string errorHint;  // Optional second line shown below errorMessage on the ERROR screen
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  // Downloaded-file index for the html2xtc "already downloaded" marker (server.verifyTls only).
  // Key: 6-hex-digit id-hash suffix (see appendIdHashSuffix in the .cpp), value: full SD path.
  // Rebuilt from a single /Html2Xtc directory listing in fetchFeed(); never touched by render()
  // so render() stays free of SD I/O. EPUB downloads (saved to the SD root, not /Html2Xtc) are
  // out of scope for this marker -- html2xtc only ever serves XTC.
  std::unordered_map<std::string, std::string> downloadedByHash;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  // When server.verifyTls, makes sure the clock is synced (NTP) before any TLS-verified request
  // is made -- a not-yet-synced clock makes every certificate look not-yet-valid. On failure,
  // sets state=ERROR with a time-sync-specific message and returns false; TLS verification is
  // never silently disabled as a fallback. No-op (returns true) when !server.verifyTls.
  bool ensureTimeSyncedIfNeeded();
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  // Populates downloadedByHash from a single listing of /Html2Xtc. Only called when
  // server.verifyTls (html2xtc); a no-op directory listing for generic OPDS servers would be
  // wasted SD I/O with no matching entries anyway.
  void scanDownloadedFiles();
  // Returns the full SD path of a previously downloaded file matching this book's id hash, or
  // "" if book.id is empty or no match was found. Pure map lookup -- safe to call from render().
  std::string downloadedPathFor(const OpdsEntry& book) const;
  bool preventAutoSleep() override { return true; }
};
