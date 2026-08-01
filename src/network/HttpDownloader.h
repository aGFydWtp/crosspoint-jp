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
   * The distinction matters because a TLS handshake needs ~45-55KB of
   * contiguous heap on this device (MBEDTLS_SSL_IN/OUT_CONTENT_LEN are 16KB
   * each and CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN is off). Spending an extra
   * 2.5KB of pre-handshake heap on every request pushes the fragmented cases
   * over the edge — see the Aozora author listing, which failed with
   * ESP_ERR_HTTP_CONNECT at ~56KB free.
   */
  enum class BufferProfile {
    COMPACT,  // 2048 / 512 - everything except the GitHub release CDN
    LARGE,    // 4096 / 1024 - long signed redirect URLs
  };

  // Last HTTP status code observed by runGet(). Fork-only: activities
  // (Aozora / FontDownload) surface this in their error messages for
  // diagnostics. Positive value = HTTP response code, 0 = never set / no
  // response received. Not thread-safe (single-threaded HTTP path).
  static int lastHttpCode;

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
