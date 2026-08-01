#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Size class for the esp_http_client RX/TX buffers, which are allocated up
   * front and held for the whole connection.
   *
   * COMPACT is the default. LARGE exists only for GitHub's release CDN: the
   * redirect target (release-assets.githubusercontent.com) is a signed URL
   * whose path+query runs 700-900 bytes, so the redirected GET's request line
   * overflows a 512-byte TX buffer and the reopen fails before any byte
   * arrives. esp_http_client reports that as ESP_FAIL ("Out of buffer" in
   * esp_http_client_request_send), which surfaces as http=1 in the activities'
   * diagnostics — not as a connect error.
   *
   * The distinction matters because a TLS handshake still needs a ~16.5KB
   * contiguous block for the inbound record buffer plus ~4KB for the outbound
   * one (platformio.ini sets CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN with
   * IN=16384 / OUT=4096). Spending an extra 2.5KB of pre-handshake heap on
   * every request pushes the fragmented cases over the edge — see the Aozora
   * author listing, which failed with ESP_ERR_HTTP_CONNECT at ~56KB free.
   */
  enum class BufferProfile {
    COMPACT,  // 2048 / 512  - everything except the GitHub release CDN
    LARGE,    // 2048 / 1536 - long signed redirect URLs; see the .cpp for the
              //               measurements behind both numbers
  };

  // Last HTTP status code observed by runGet(). Fork-only: activities
  // (Aozora / FontDownload) surface this in their error messages for
  // diagnostics. Positive value = HTTP response code, 0 = never set / no
  // response received. Not thread-safe (single-threaded HTTP path).
  static int lastHttpCode;

  // Last TLS-layer failure behind an ESP_ERR_HTTP_CONNECT, captured with
  // esp_http_client_get_and_clear_last_tls_error(). Fork-only diagnostics:
  // ESP_ERR_HTTP_CONNECT alone cannot tell a name-resolution failure from a
  // refused socket from a handshake that ran out of contiguous heap, and those
  // need completely different fixes.
  //
  // Negative values are raw mbedtls errors, positive ones esp-tls errors; the
  // ranges are disjoint, so a single int is unambiguous.
  //
  //   lastTlsError  0       = nothing was recorded at the TLS layer at all
  //                 -32512  = MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00) -> out of
  //                           contiguous heap for the 16.5KB/4KB SSL buffers
  //                 32769   = ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME (0x8001)
  //                 32770   = ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET (0x8002)
  //                 32772   = ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST (0x8004)
  //                 32774   = ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT (0x8006)
  //                 other   = see esp_tls_errors.h / mbedtls' ssl.h
  //   lastTlsFlags  non-zero     = certificate verification failed; the bits are
  //                                mbedtls' MBEDTLS_X509_BADCERT_* set
  static int lastTlsError;
  static int lastTlsFlags;

  // Which esp_crt_bundle failure produced a tls=12288. mbedtls collapses every
  // certificate rejection into MBEDTLS_ERR_X509_FATAL_ERROR, so this is the
  // only way to tell them apart without a serial console:
  //   0 = no certificate failure recorded
  //   1 = bundle never attached
  //   2 = parsing the root's public key failed  -> out of memory
  //   3 = key type / signature algorithm mismatch
  //   4 = hash algorithm unsupported in this build
  //   5 = hashing the certificate body failed
  //   6 = signature rejected, or an MPI allocation failed inside the verify
  //   7 = only the generic wrapper was seen (should not happen; 2-6 precede it)
  //   8 = no matching trusted root in the bundle
  static int lastCrtDiag;

  // The mbedtls error code carried by the "PK parse/verify failed with error
  // 0x%x" log line, positive (as logged). 0x4300 is
  // MBEDTLS_ERR_RSA_VERIFY_FAILED — the signature was genuinely rejected;
  // 0x0010 is MBEDTLS_ERR_MPI_ALLOC_FAILED — it ran out of memory instead.
  static int lastCrtErr;

  // Free heap and largest free block, in KB, sampled *inside* the failing
  // handshake rather than after it unwinds. This is the only view of the state
  // the failed allocation actually faced.
  static int lastCrtHeapKb;
  static int lastCrtBlkKb;

  // Free heap / largest block at the start of the request, before any
  // esp_http_client or mbedtls allocation. The gap to lastCrtHeapKb is what the
  // connection cost.
  static int lastPreHeapKb;
  static int lastPreBlkKb;

  // Captured by a heap_caps failed-allocation callback, i.e. at the instant the
  // allocator gave up rather than after the caller unwound. lastFailSize is the
  // requested byte count, lastFailBlk the largest block that existed then (in
  // bytes — at these sizes kilobytes hide the answer).
  static int lastFailSize;
  static int lastFailFreeKb;
  static int lastFailBlk;

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", BufferProfile buffers = BufferProfile::COMPACT);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      BufferProfile buffers = BufferProfile::COMPACT);
};
