#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "SecureNetworkClient.h"
#include "util/UrlUtils.h"

int HttpDownloader::lastHttpCode = 0;

namespace {

// Maximum time the streaming read loop waits without receiving a single body byte before it
// gives up. Only reached when the peer stops sending mid-body but keeps the socket open; a
// well-behaved response either delivers Content-Length bytes or closes the connection.
constexpr uint32_t STREAM_IDLE_TIMEOUT_MS = 10000;

// Builds the NetworkClient appropriate for the URL/verifyTls combination.
// - https + verifyTls: SecureNetworkClient with the default CA bundle attached (chain + hostname
//   verification). *outSecureForError is set so the caller can pull a diagnostic message on failure.
// - https + !verifyTls: NetworkClientSecure with setInsecure() (unchanged legacy behavior).
// - http (plain): NetworkClient, unless verifyTls was requested -- html2xtc is https-only, so
//   verifyTls=true against a plain http:// URL is a caller error and yields nullptr.
std::unique_ptr<NetworkClient> makeHttpClient(const std::string& url, bool verifyTls,
                                              SecureNetworkClient** outSecureForError) {
  if (outSecureForError) *outSecureForError = nullptr;

  if (UrlUtils::isHttpsUrl(url)) {
    if (verifyTls) {
      auto* secureClient = new SecureNetworkClient();
      secureClient->useDefaultCertBundle();
      secureClient->setHandshakeTimeout(20);
      if (outSecureForError) *outSecureForError = secureClient;
      return std::unique_ptr<NetworkClient>(secureClient);
    }
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    secureClient->setHandshakeTimeout(20);
    return std::unique_ptr<NetworkClient>(secureClient);
  }

  if (verifyTls) {
    LOG_ERR("HTTP", "verifyTls requested for a non-HTTPS URL: %s", url.c_str());
    return nullptr;
  }
  return std::unique_ptr<NetworkClient>(new NetworkClient());
}

// Logs the mbedtls-level diagnostic for a TLS/connection failure. Only meaningful when
// secureForError is non-null (i.e. verifyTls was in effect) and the HTTP layer reported a
// transport-level failure (httpCode <= 0) rather than a genuine HTTP status code (>= 100).
bool logTlsFailureIfAny(SecureNetworkClient* secureForError, int httpCode) {
  if (!secureForError || httpCode > 0) {
    return false;
  }
  char errBuf[128];
  secureForError->lastError(errBuf, sizeof(errBuf));
  LOG_ERR("HTTP", "TLS/connection error: %s (code=%d)", errBuf, httpCode);
  return true;
}

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress)
      : file_(file), total_(total), progress_(std::move(progress)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    if (aborted_) {
      // A previous call already requested cancellation via a short-write return. HTTPClient's
      // writeToStreamDataBlock() retries the remainder of the same chunk once after a short
      // write before giving up, so this can be invoked again after aborted_ is set. Reporting a
      // short write again is harmless: downloadToFile() discards the entire temp file on error.
      return 0;
    }

    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    // Reported even when total_ is 0 (a chunked response carries no Content-Length). Gating on a
    // known total used to make such downloads both progress-less and impossible to cancel, since
    // cancellation is signalled through this very callback. Callers already treat total == 0 as
    // "length unknown" and skip their progress bar.
    if (progress_) {
      if (!progress_(downloaded_, total_)) {
        // Cancellation requested: report a short write so HTTPClient::writeToStreamDataBlock
        // treats this as a stream error, stops the connection (via returnError()), and unwinds
        // back to downloadToFile() with a negative writeResult.
        aborted_ = true;
        return 0;
      }
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }
  bool aborted() const { return aborted_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  bool aborted_ = false;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, bool verifyTls) {
  // Streaming variant: identical connection setup to the std::string overload,
  // but pushes body chunks into onData instead of buffering them. Used by the
  // OTA release-check path where TLS session heap + full-body buffer would OOM.
  SecureNetworkClient* secureForError = nullptr;
  std::unique_ptr<NetworkClient> client = makeHttpClient(url, verifyTls, &secureForError);
  if (!client) {
    lastHttpCode = TLS_ERROR_CODE;
    return false;
  }
  HTTPClient http;

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("HTTP", "FetchStream: %s (heap=%d)", url.c_str(), ESP.getFreeHeap());
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  if (logTlsFailureIfAny(secureForError, httpCode)) {
    lastHttpCode = TLS_ERROR_CODE;
  }
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "FetchStream failed: %d", httpCode);
    http.end();
    return false;
  }

  NetworkClient* stream = http.getStreamPtr();
  // getStreamPtr() returns nullptr when connected() is false, i.e. the peer closed the socket
  // (or the TLS session dropped) after the 200 header but before sending any body byte. The read
  // loop below dereferences stream unconditionally, so bail out here instead of panicking.
  if (!stream) {
    LOG_ERR("HTTP", "FetchStream: no stream (connection closed before body)");
    http.end();
    lastHttpCode = -904;  // Custom code: stream unavailable after 200 response
    return false;
  }
  // Content-Length bounds the body exactly. Without it the loop can only stop when the peer
  // closes the socket -- and HTTPClient sends Connection: keep-alive by default, so a server
  // that holds the connection open after the body would leave available()==0 &&
  // connected()==true forever. A negative/zero size means chunked or unknown length; that case
  // still relies on connected(), with STREAM_IDLE_TIMEOUT_MS as the backstop.
  const int contentLen = http.getSize();
  const size_t expected = contentLen > 0 ? static_cast<size_t>(contentLen) : 0;

  uint8_t buf[512];
  size_t total = 0;
  bool aborted = false;
  bool timedOut = false;
  uint32_t lastDataMs = millis();
  while (stream->available() || stream->connected()) {
    if (expected > 0 && total >= expected) break;

    int avail = stream->available();
    if (avail <= 0) {
      // Unsigned subtraction so a millis() rollover still yields the true elapsed interval.
      if (static_cast<uint32_t>(millis() - lastDataMs) >= STREAM_IDLE_TIMEOUT_MS) {
        timedOut = true;
        break;
      }
      delay(1);  // Yield so the task watchdog is fed while waiting for more body bytes.
      continue;
    }
    size_t toRead = (avail < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(avail) : sizeof(buf);
    if (expected > 0 && toRead > expected - total) {
      toRead = expected - total;  // Never read past the body into a pipelined keep-alive response.
    }
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead <= 0) break;
    if (onData && !onData(buf, static_cast<size_t>(bytesRead))) {
      aborted = true;
      break;
    }
    total += bytesRead;
    lastDataMs = millis();
  }
  http.end();

  if (aborted) {
    LOG_DBG("HTTP", "FetchStream aborted by callback after %zu bytes", total);
    return false;
  }
  if (timedOut) {
    LOG_ERR("HTTP", "FetchStream stalled after %zu bytes (contentLen=%d)", total, contentLen);
    lastHttpCode = -902;  // Custom code: no body data received within STREAM_IDLE_TIMEOUT_MS
    return false;
  }
  if (total == 0) {
    LOG_ERR("HTTP", "FetchStream: empty body");
    lastHttpCode = -901;
    return false;
  }
  // The loop also exits when the peer closes the socket, which happens before the body is
  // complete if the connection drops mid-response. Without this check a truncated body would be
  // reported as success and the caller would parse a partial payload (e.g. a release JSON whose
  // download URL is cut in half). Only enforceable when Content-Length was present; expected == 0
  // means chunked/unknown length, where the connected() + idle-timeout logic above is all we have.
  if (expected > 0 && total < expected) {
    LOG_ERR("HTTP", "FetchStream truncated: got %zu bytes, expected %zu", total, expected);
    lastHttpCode = -903;  // Custom code: body shorter than Content-Length
    return false;
  }

  LOG_DBG("HTTP", "FetchStream success: %zu bytes", total);
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, bool verifyTls) {
  SecureNetworkClient* secureForError = nullptr;
  std::unique_ptr<NetworkClient> client = makeHttpClient(url, verifyTls, &secureForError);
  if (!client) {
    lastHttpCode = TLS_ERROR_CODE;
    return false;
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("HTTP", "Free heap before GET: %d", ESP.getFreeHeap());
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("HTTP", "GET result: %d, free heap: %d", httpCode, ESP.getFreeHeap());
  if (logTlsFailureIfAny(secureForError, httpCode)) {
    lastHttpCode = TLS_ERROR_CODE;
  }
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  const int writeResult = http.writeToStream(&outContent);
  http.end();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream failed: %d", writeResult);
    lastHttpCode = writeResult;
    return false;
  }

  LOG_DBG("HTTP", "Fetch success: %d bytes", writeResult);
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, bool verifyTls) {
  // Direct string fetch: avoids StreamString and writeToStream issues.
  SecureNetworkClient* secureForError = nullptr;
  std::unique_ptr<NetworkClient> client = makeHttpClient(url, verifyTls, &secureForError);
  if (!client) {
    lastHttpCode = TLS_ERROR_CODE;
    return false;
  }
  HTTPClient http;

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("HTTP", "FetchStr: %s (heap=%d)", url.c_str(), ESP.getFreeHeap());
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  if (logTlsFailureIfAny(secureForError, httpCode)) {
    lastHttpCode = TLS_ERROR_CODE;
  }
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "FetchStr failed: %d", httpCode);
    http.end();
    return false;
  }

  // Read body in small chunks to avoid large single allocation.
  // TLS buffers (~40KB) are held during the connection, leaving limited heap.
  NetworkClient* stream = http.getStreamPtr();
  const int contentLen = http.getSize();
  outContent.clear();
  if (contentLen > 0) {
    outContent.reserve(contentLen);
  }

  char buf[512];
  while (stream->available() || stream->connected()) {
    int avail = stream->available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int toRead = (avail < static_cast<int>(sizeof(buf))) ? avail : static_cast<int>(sizeof(buf));
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead > 0) {
      outContent.append(buf, bytesRead);
    } else {
      break;
    }
  }
  http.end();

  if (outContent.empty()) {
    LOG_ERR("HTTP", "FetchStr: empty body (contentLen=%d)", contentLen);
    lastHttpCode = -901;
    return false;
  }

  LOG_DBG("HTTP", "FetchStr success: %zu bytes", outContent.size());
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, int timeoutMs,
                                                             const std::string& username, const std::string& password,
                                                             HttpResponseMetadata* outMetadata, bool verifyTls) {
  SecureNetworkClient* secureForError = nullptr;
  std::unique_ptr<NetworkClient> client = makeHttpClient(url, verifyTls, &secureForError);
  if (!client) {
    lastHttpCode = TLS_ERROR_CODE;
    return TLS_ERROR;
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (timeoutMs > 0) {
    http.setTimeout(timeoutMs);
  }
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  // Headers must be registered before GET() or header() will always return empty afterwards.
  if (outMetadata) {
    static const char* kCollectedHeaders[] = {"Content-Type", "Content-Disposition"};
    http.collectHeaders(kCollectedHeaders, 2);
  }

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  const bool tlsFailure = logTlsFailureIfAny(secureForError, httpCode);
  if (tlsFailure) {
    lastHttpCode = TLS_ERROR_CODE;
  }
  if (outMetadata) {
    outMetadata->statusCode = httpCode;
    outMetadata->contentType = http.header("Content-Type").c_str();
    outMetadata->contentDisposition = http.header("Content-Disposition").c_str();
  }
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return tlsFailure ? TLS_ERROR : HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }
  if (outMetadata) {
    outMetadata->contentLength = contentLength;
  }

  // Download into a temporary ".part" file and rename it to destPath only after
  // all verification passes. This keeps destPath atomic: it either holds the old
  // complete file or the new complete file, never a truncated download.
  const std::string partPath = destPath + ".part";

  // Clean up any stale .part file left over from a previous interrupted download.
  if (Storage.exists(partPath.c_str())) {
    Storage.remove(partPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Let HTTPClient handle chunked decoding and stream body bytes into the file.
  FileWriteStream fileStream(file, contentLength, progress);
  const int writeResult = http.writeToStream(&fileStream);

  file.close();
  http.end();

  if (writeResult < 0) {
    if (fileStream.aborted()) {
      LOG_INF("HTTP", "Download cancelled by progress callback (len=%zu, downloaded=%zu)", contentLength,
              fileStream.downloaded());
      Storage.remove(partPath.c_str());
      return ABORTED;
    }
    LOG_ERR("HTTP", "writeToStream error: %d (len=%zu)", writeResult, contentLength);
    lastHttpCode = writeResult;  // Store writeToStream error code for diagnostics
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }

  // Defensive: if the framework ever tolerates the abort's short write and reports success,
  // the file is still truncated -- never let an aborted download pass as OK.
  if (fileStream.aborted()) {
    LOG_INF("HTTP", "Download cancelled by progress callback (len=%zu, downloaded=%zu)", contentLength,
            fileStream.downloaded());
    Storage.remove(partPath.c_str());
    return ABORTED;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    lastHttpCode = -900;  // Custom code: SD write failure
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    lastHttpCode = -901;  // Custom code: no data
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }

  // All checks passed: move the .part file into place. SdFat's rename() opens the
  // target with O_EXCL semantics and fails if it already exists, so remove the old
  // destination first.
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    // Keep the verified .part file: deleting it here would lose both the old file
    // (removed above) and the fully downloaded data. The stale-.part cleanup at the
    // top of this function reclaims it on the next download attempt.
    LOG_ERR("HTTP", "Rename failed: %s -> %s", partPath.c_str(), destPath.c_str());
    return FILE_ERROR;
  }

  return OK;
}
