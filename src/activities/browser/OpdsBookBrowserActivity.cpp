#include "OpdsBookBrowserActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <functional>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TimeSync.h"
#include "util/DownloadFormat.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

// Single reusable temp-download path: only one OPDS download runs at a time (the activity blocks
// input while state == DOWNLOADING), so this doesn't need to be unique per-download.
constexpr const char* kTempDownloadPath = "/opds_download.tmp";
constexpr const char* kHtml2XtcDir = "/Html2Xtc";

std::string fallbackBaseName(const OpdsEntry& book) {
  return (book.author.empty() ? "" : book.author + " - ") + book.title;
}

// Deterministic short suffix derived from the OPDS entry id, used to disambiguate filename
// collisions while keeping repeat downloads of the same item idempotent.
std::string appendIdHashSuffix(const std::string& base, const std::string& id) {
  char suffix[10];
  snprintf(suffix, sizeof(suffix), "_%06zx", std::hash<std::string>{}(id) & 0xFFFFFFu);
  return base + suffix;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  errorHint.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  // Clean up a temp file left behind by a download that never completed (e.g. crash, power loss).
  Storage.remove(kTempDownloadPath);

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
  entries.clear();
  navigationHistory.clear();
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        if (!ensureTimeSyncedIfNeeded()) return;
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    if (!entries.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    if (!errorHint.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, errorHint.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      std::string displayText = (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : entry.title;
      if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  // Clear any hint left over from a previous error (e.g. a TLS failure) up front, so a later
  // failure in this call that doesn't set its own hint can't show a stale, unrelated one.
  errorHint.clear();

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    errorHint.clear();
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password, server.verifyTls)) {
      state = BrowserState::ERROR;
      if (HttpDownloader::lastHttpCode == HttpDownloader::TLS_ERROR_CODE) {
        errorMessage = tr(STR_ERROR_TLS_VERIFICATION_FAILED);
        errorHint = tr(STR_ERROR_TLS_CHECK_HINT);
      } else {
        errorMessage = tr(STR_FETCH_FEED_FAILED);
        errorHint.clear();
      }
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, "", ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, "", ""});
  }

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), kTempDownloadPath);

  // Clean up a stale temp file left behind by a previous interrupted download.
  Storage.remove(kTempDownloadPath);

  HttpDownloader::HttpResponseMetadata metadata;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, kTempDownloadPath,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      0, server.username, server.password, &metadata, server.verifyTls);

  if (result != HttpDownloader::OK) {
    LOG_ERR("OPDS", "Download failed: err=%d http=%d", static_cast<int>(result), HttpDownloader::lastHttpCode);
    Storage.remove(kTempDownloadPath);
    state = BrowserState::ERROR;
    if (result == HttpDownloader::TLS_ERROR) {
      errorMessage = tr(STR_ERROR_TLS_VERIFICATION_FAILED);
      errorHint = tr(STR_ERROR_TLS_CHECK_HINT);
    } else {
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      errorHint.clear();
    }
    requestUpdate();
    return;
  }

  // Format is decided only after the download completes: Content-Disposition filename extension
  // (preferred), then the response Content-Type, then (if that's inconclusive, e.g. a generic
  // application/octet-stream) the OPDS entry's own media type, then the href extension.
  const std::string dispositionFilename =
      DownloadFormatUtils::parseContentDispositionFilename(metadata.contentDisposition);
  DownloadFormat format = DownloadFormatUtils::detectFormat(dispositionFilename, metadata.contentType, book.href);
  if (format == DownloadFormat::UNKNOWN) {
    format = DownloadFormatUtils::detectFormat(dispositionFilename, book.mediaType, book.href);
  }

  if (format == DownloadFormat::UNKNOWN) {
    LOG_ERR("OPDS", "Unrecognized download format (contentType=%s, entryMediaType=%s, href=%s)",
            metadata.contentType.c_str(), book.mediaType.c_str(), book.href.c_str());
    Storage.remove(kTempDownloadPath);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_UNSUPPORTED_DOWNLOAD_FORMAT);
    errorHint.clear();
    requestUpdate();
    return;
  }

  const char* extension = DownloadFormatUtils::extensionForFormat(format);
  std::string destPath;

  if (format == DownloadFormat::EPUB) {
    // Unchanged from pre-Phase2 behavior: fixed root-directory "<author> - <title>.epub" path,
    // never the Content-Disposition filename, so existing OPDS users re-downloading a book keep the
    // same filename (and thus reading progress) instead of silently landing on a different file.
    const std::string baseName = StringUtils::sanitizeFilename(fallbackBaseName(book));
    destPath = "/" + baseName + extension;
    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
  } else {
    const std::string destDir = std::string(kHtml2XtcDir) + "/";
    if (!Storage.exists(kHtml2XtcDir) && !Storage.mkdir(kHtml2XtcDir)) {
      LOG_ERR("OPDS", "Failed to create %s", kHtml2XtcDir);
      Storage.remove(kTempDownloadPath);
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      errorHint.clear();
      requestUpdate();
      return;
    }

    const std::string baseName =
        dispositionFilename.empty()
            ? StringUtils::sanitizeFilename(fallbackBaseName(book))
            : StringUtils::sanitizeFilename(DownloadFormatUtils::stripExtension(dispositionFilename));
    // Always append the id-hash suffix (when we have an id) instead of only doing so on collision:
    // an exists-then-decide branch means the same item's first download lands on the bare path and
    // every subsequent re-download of that same item collides with it and gets pushed onto the
    // suffixed path, leaving the original as an orphan and creating a new file per re-download.
    // Making the suffix unconditional keeps a given item's path deterministic across downloads
    // (idempotent overwrite below) while still avoiding collisions between different items that
    // happen to share a title (they hash to different suffixes).
    const std::string finalBaseName = book.id.empty() ? baseName : appendIdHashSuffix(baseName, book.id);
    destPath = destDir + finalBaseName + extension;
    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
  }

  if (!Storage.rename(kTempDownloadPath, destPath.c_str())) {
    LOG_ERR("OPDS", "Rename failed: %s -> %s", kTempDownloadPath, destPath.c_str());
    Storage.remove(kTempDownloadPath);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    errorHint.clear();
    requestUpdate();
    return;
  }

  LOG_DBG("OPDS", "Downloaded: %s", destPath.c_str());

  if (format == DownloadFormat::EPUB) {
    Epub(destPath, "/.crosspoint").clearCache();
  }

  state = BrowserState::BROWSING;
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    if (!ensureTimeSyncedIfNeeded()) return;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

bool OpdsBookBrowserActivity::ensureTimeSyncedIfNeeded() {
  if (!server.verifyTls || TimeSync::isTimeValid()) {
    return true;
  }

  // NTP sync happens before the first TLS-verified request: a not-yet-synced clock (post-boot,
  // no RTC battery) makes every certificate's notBefore check fail. Blocking here is consistent
  // with fetchFeed()/downloadBook() already blocking the activity loop for network I/O.
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  if (TimeSync::ensureSynced(10000)) {
    return true;
  }

  LOG_ERR("OPDS", "NTP sync failed; refusing to proceed with TLS-verified server");
  state = BrowserState::ERROR;
  errorMessage = tr(STR_ERROR_TIME_SYNC_FAILED);
  errorHint.clear();
  requestUpdate();
  return false;
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    if (!ensureTimeSyncedIfNeeded()) return;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    errorHint.clear();
    requestUpdate();
  }
}
