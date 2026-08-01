#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>

#include <functional>
#include <string>

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <cstdarg>
#include <cstdlib>
#include <cstring>

/*
 * When esp_crt_bundle.h is included, it points to the wrong header file
 * (something under WiFiClientSecure) because our framework is based on the
 * Arduino platform. To manage this obstacle, don't include anything, just
 * extern and it will point to the correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}
#endif

// fork-only: activities show these in error messages for diagnostics
int HttpDownloader::lastHttpCode = 0;
int HttpDownloader::lastTlsError = 0;
int HttpDownloader::lastTlsFlags = 0;
int HttpDownloader::lastCrtDiag = 0;
int HttpDownloader::lastCrtErr = 0;
int HttpDownloader::lastCrtHeapKb = 0;
int HttpDownloader::lastCrtBlkKb = 0;
int HttpDownloader::lastPreHeapKb = 0;
int HttpDownloader::lastPreBlkKb = 0;
int HttpDownloader::lastFailSize = 0;
int HttpDownloader::lastFailFreeKb = 0;
int HttpDownloader::lastFailBlk = 0;

#if !defined(FREEINK_NET_WOLFSSL)
namespace {
/*
 * tls=12288 is MBEDTLS_ERR_X509_FATAL_ERROR, but that value says almost
 * nothing on its own: mbedtls rewrites the bundle callback's
 * MBEDTLS_ERR_X509_CERT_VERIFY_FAILED into it (x509_crt.c, "prevent misuse of
 * the vrfy callback"), so an empty bundle, an unknown root and a failed
 * signature check all arrive as the same number. esp_crt_bundle does
 * distinguish them — but only in its log output, and the USB Serial/JTAG link
 * on this device drops out exactly when a failure is being reproduced.
 *
 * So tap the log stream instead. ESP_LOGx passes its *format string* to the
 * vprintf hook with the arguments still unformatted, and every marker below is
 * literal text in that format string, so the common case costs only a few
 * strstr() calls before delegating to whatever handler was there before. Only
 * the two markers that carry an error code render the line, and only on the
 * failure path.
 */
/*
 * Ordered inner-reason-first. esp_crt_check_signature() logs the specific cause
 * and then its caller logs the generic "Certificate matched but signature
 * verification failed", so the *first* marker seen in a run is always the
 * informative one — hence first-write-wins below. An earlier version kept the
 * lowest code instead, which let the generic message mask the specific one and
 * made a bare crt=2 ambiguous.
 */
constexpr const char* CRT_MARKERS[] = {
    "No certificates in bundle",  // 1: bundle never attached
    "PK parse failed",            // 2: parsing the root's public key failed (OOM lands here)
    "Unsuitable public key",      // 3: key type does not match the signature algorithm
    "Unknown message digest",     // 4: hash algorithm unsupported in this build
    "MD failed",                  // 5: hashing the cert body failed
    "PK verify failed",           // 6: signature mathematically rejected (or MPI alloc failure)
    "Certificate matched but signature verification failed",  // 7: generic wrapper for 2-6
    "Failed to verify certificate",  // 8: generic tail; alone it means "no matching root in bundle"
};
constexpr int CRT_MARKER_COUNT = 8;

vprintf_like_t g_prevLogHandler = nullptr;

int crtLogHook(const char* fmt, va_list args) {
  if (fmt) {
    for (int i = 0; i < CRT_MARKER_COUNT; i++) {
      if (strstr(fmt, CRT_MARKERS[i]) != nullptr) {
        // First write wins: the specific reason is logged before the generic one.
        if (HttpDownloader::lastCrtDiag == 0) HttpDownloader::lastCrtDiag = i + 1;
        // "PK parse failed with error 0x%x" and "PK verify failed with error
        // 0x%x" carry the mbedtls code, which is the difference between a
        // genuine signature mismatch (0x4300 = MBEDTLS_ERR_RSA_VERIFY_FAILED)
        // and an allocation that died mid-verify (0x0010 =
        // MBEDTLS_ERR_MPI_ALLOC_FAILED). Format the line only on this rare
        // failure path and scrape the number back out, rather than walking the
        // va_list — the log macro's argument layout is an IDF implementation
        // detail, but the rendered text is not.
        if (strstr(fmt, "with error 0x") != nullptr && HttpDownloader::lastCrtErr == 0) {
          char line[160];
          va_list copy;
          va_copy(copy, args);
          const int written = vsnprintf(line, sizeof(line), fmt, copy);
          va_end(copy);
          if (written > 0) {
            const char* at = strstr(line, "error 0x");
            if (at) HttpDownloader::lastCrtErr = static_cast<int>(strtol(at + 8, nullptr, 16));
          }
          // Sample the heap here, not after the request unwinds. This hook runs
          // synchronously from the failing mbedtls call, still inside the
          // handshake, so these are the numbers the allocation actually saw —
          // the post-mortem values an activity prints have already had ~40KB of
          // TLS structures returned to the arena and look far healthier than
          // reality.
          HttpDownloader::lastCrtHeapKb = static_cast<int>(ESP.getFreeHeap() / 1024);
          HttpDownloader::lastCrtBlkKb = static_cast<int>(ESP.getMaxAllocHeap() / 1024);
        }
        break;
      }
    }
  }
  return g_prevLogHandler ? g_prevLogHandler(fmt, args) : 0;
}

// Fires from inside the allocator at the moment a request cannot be satisfied —
// unlike the log hook, which only runs once the failing call has unwound and its
// siblings have already been freed. The requested size and the largest block
// available right then are the two numbers that decide whether the problem is
// the total, the layout, or a size nobody expected.
void heapFailHook(size_t size, uint32_t /*caps*/, const char* /*fn*/) {
  if (HttpDownloader::lastFailSize != 0) return;  // keep the first failure of the request
  HttpDownloader::lastFailSize = static_cast<int>(size);
  HttpDownloader::lastFailFreeKb = static_cast<int>(ESP.getFreeHeap() / 1024);
  HttpDownloader::lastFailBlk = static_cast<int>(ESP.getMaxAllocHeap());
}

void ensureDiagnostics() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  g_prevLogHandler = esp_log_set_vprintf(&crtLogHook);
  heap_caps_register_failed_alloc_callback(&heapFailHook);
}
}  // namespace
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers; TX must fit the whole request line.
// fork: upstream shrank these to 2048/512, but upstream routes OTA/large
// downloads through wolfSSL (runGetWolf) — here runGet carries them too, so
// COMPACT keeps upstream's sizes and LARGE exists for GitHub's release CDN
// alone. See BufferProfile in the header for why the other callers must stay on
// the smaller buffers.
// RX is profile-independent: the response header block from the GitHub release
// CDN measures 840 bytes with a 65-byte longest line, and runGet() streams the
// body in READ_CHUNK pieces, so a bigger RX buys nothing on either profile.
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF_COMPACT = 512;
// LARGE is a TX problem, not an RX one, and both sides are now sized from
// measurements against release-assets.githubusercontent.com rather than the
// round numbers inherited from the OTA work:
//
// LARGE is a TX-only distinction, sized from measurement: the signed redirect's
// request line alone is 892 bytes. Add Host (44) and a User-Agent carrying
// CROSSPOINT_VERSION — 81 bytes on a dev build, where the version string is
// "0.1.14-dev-<branch>-<sha>" — plus esp_http_client's own headers, and the
// worst case lands just over 1024. Overflowing it fails the request with
// ESP_FAIL before a byte is sent (http=1), so 1536 leaves margin for long
// branch names.
constexpr int HTTP_TX_BUF_LARGE = 1536;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

#if !defined(FREEINK_NET_WOLFSSL)
// Pull the TLS-layer reason out of the client after open() reported
// ESP_ERR_HTTP_CONNECT. The transport clears it on read, so call this once, and
// only on the failure path. Leaves the fields at 0 when the failure never
// reached esp-tls (plain http, DNS, refused socket).
void captureTlsError(esp_http_client_handle_t client) {
  int mbedtlsCode = 0;
  int flags = 0;
  // The return value is esp-tls' own error (ESP_ERR_ESP_TLS_*, 0x8000-based),
  // which is what separates "DNS never resolved" from "the socket was refused";
  // the out-params carry the underlying mbedtls code. Both are meaningful, and
  // the call wipes the handle, so read it exactly once and keep whichever is
  // set. The two ranges are disjoint — mbedtls codes are negative, esp-tls ones
  // positive — so one int can hold either without ambiguity.
  const esp_err_t layerErr = esp_http_client_get_and_clear_last_tls_error(client, &mbedtlsCode, &flags);
  if (layerErr == ESP_ERR_INVALID_STATE) return;  // plain http: no TLS handle to report on

  HttpDownloader::lastTlsError = mbedtlsCode != 0 ? mbedtlsCode : static_cast<int>(layerErr);
  HttpDownloader::lastTlsFlags = flags;
  if (HttpDownloader::lastTlsError || flags) {
    LOG_ERR("HTTP", "TLS error: esp_tls=0x%X mbedtls=%d (-0x%X) flags=0x%X", layerErr, mbedtlsCode, -mbedtlsCode,
            flags);
  }
}
#endif

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found").
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&http, &sink](const uint8_t* data, size_t len) {
          if (http.getStatus() != 200) return true;
          if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, HttpDownloader::BufferProfile buffers) {
  const bool large = buffers == HttpDownloader::BufferProfile::LARGE;
  ensureDiagnostics();
  // Baseline for the diagnostics: the state going in, before esp_http_client or
  // mbedtls have allocated anything for this request. Paired with lastCrtHeapKb
  // (sampled inside a failing handshake) it says how much the connection itself
  // consumed, which is the number that decides whether there is anything left
  // to reclaim on our side.
  HttpDownloader::lastPreHeapKb = static_cast<int>(ESP.getFreeHeap() / 1024);
  HttpDownloader::lastPreBlkKb = static_cast<int>(ESP.getMaxAllocHeap() / 1024);
  HttpDownloader::lastFailSize = 0;
  HttpDownloader::lastFailFreeKb = 0;
  HttpDownloader::lastFailBlk = 0;
  // reset before each attempt so activities' diagnostics reflect this call only
  HttpDownloader::lastHttpCode = 0;
  HttpDownloader::lastTlsError = 0;
  HttpDownloader::lastTlsFlags = 0;
  HttpDownloader::lastCrtDiag = 0;
  HttpDownloader::lastCrtErr = 0;
  HttpDownloader::lastCrtHeapKb = 0;
  HttpDownloader::lastCrtBlkKb = 0;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = large ? HTTP_TX_BUF_LARGE : HTTP_TX_BUF_COMPACT;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    HttpDownloader::lastHttpCode = -static_cast<int>(err);  // fork: 負値=esp_err で診断表示
    captureTlsError(client);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    HttpDownloader::lastHttpCode = status;  // fork: redirect 中の失敗でも直前の status を残す
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      HttpDownloader::lastHttpCode = -static_cast<int>(err);  // fork: 負値=esp_err で診断表示
      captureTlsError(client);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }
  HttpDownloader::lastHttpCode = status;

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
#endif  // !FREEINK_NET_WOLFSSL

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink,
                                           HttpDownloader::BufferProfile buffers) {
#if defined(FREEINK_NET_WOLFSSL)
  // wolfSSL sizes its own buffers; the profile only applies to esp_http_client.
  (void)buffers;
  return runGetWolf(url, username, password, sink);
#else
  return runGet(url, username, password, sink, buffers);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink, BufferProfile::COMPACT) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink, BufferProfile::COMPACT) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, BufferProfile buffers) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink, buffers) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             BufferProfile buffers) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetSecure(url, username, password, sink, buffers);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
