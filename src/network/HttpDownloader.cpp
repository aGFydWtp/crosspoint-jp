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
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_ && total_ > 0) {
      progress_(downloaded_, total_);
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

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

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
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
    LOG_ERR("HTTP", "writeToStream error: %d (len=%zu)", writeResult, contentLength);
    lastHttpCode = writeResult;  // Store writeToStream error code for diagnostics
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    lastHttpCode = -900;  // Custom code: SD write failure
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    lastHttpCode = -901;  // Custom code: no data
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
