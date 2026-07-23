#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps NetworkClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  /// Progress callback invoked after each successful chunk write during downloadToFile().
  /// Return true to continue the download, or false to request cancellation -- the in-flight
  /// write is reported as a short write to HTTPClient, which closes the connection and causes
  /// downloadToFile() to return ABORTED (the partial temp file is removed automatically).
  using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    TLS_ERROR,
  };

  /// lastHttpCode value used to signal a TLS chain/hostname verification (or transport-level
  /// connection) failure when verifyTls=true. Distinguishes this from a genuine HTTP status code.
  static constexpr int TLS_ERROR_CODE = -950;

  /**
   * Subset of HTTP response headers, populated by downloadToFile() when an output pointer is provided.
   * etag/lastModified are intentionally omitted -- nothing in the codebase consumes them yet.
   */
  struct HttpResponseMetadata {
    int statusCode = 0;
    size_t contentLength = 0;
    std::string contentType;
    std::string contentDisposition;
  };

  /// Last HTTP status code from downloadToFile (-1 = connection failed, -11 = timeout, etc.)
  static int lastHttpCode;

  /**
   * Fetch text content from a URL with optional Basic auth credentials.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @param username Optional username for Basic auth
   * @param password Optional password for Basic auth
   * @param verifyTls When true (https only), verify the server's certificate chain and hostname
   *                  against the embedded default CA bundle instead of accepting any certificate.
   *                  Requesting verifyTls=true for a plain http:// URL is treated as an error.
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", bool verifyTls = false);

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", bool verifyTls = false);

  /**
   * Download a file to the SD card with optional Basic auth credentials.
   * @param outMetadata Optional output pointer; when non-null, Content-Type/Content-Disposition headers are
   *                    collected and the response metadata is populated regardless of success/failure.
   * @param verifyTls When true (https only), verify the server's certificate chain and hostname
   *                  against the embedded default CA bundle instead of accepting any certificate.
   *                  Requesting verifyTls=true for a plain http:// URL returns TLS_ERROR.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, int timeoutMs = 0,
                                      const std::string& username = "", const std::string& password = "",
                                      HttpResponseMetadata* outMetadata = nullptr, bool verifyTls = false);

  /**
   * Send a JSON request body via HTTP POST and capture the response body.
   * Used by REST-style JSON APIs (e.g. html2xtc device pairing) that need an arbitrary
   * Authorization header value rather than the Basic-auth username/password pair above.
   * @param jsonBody Raw JSON request body (Content-Type: application/json is set automatically).
   * @param outResponse Response body (output), populated whenever a response was received
   *                    regardless of status code, so callers can inspect error bodies too.
   * @param authorization When non-empty, sent verbatim as the Authorization header value
   *                      (e.g. "Pairing abc123", "Bearer xyz") -- no scheme is added/assumed.
   * @param verifyTls Same semantics as fetchUrl()/downloadToFile().
   * @param outRetryAfterSeconds Optional output pointer; when non-null, the Retry-After response
   *                            header is parsed as an integer number of seconds (0 if absent).
   * @return HTTP status code, or a negative error code (see TLS_ERROR_CODE) on transport failure.
   */
  static int postJson(const std::string& url, const std::string& jsonBody, std::string& outResponse,
                      const std::string& authorization = "", bool verifyTls = false,
                      int* outRetryAfterSeconds = nullptr);

  /**
   * Issue an authenticated HTTP GET expecting a JSON response body (e.g. polling a status
   * endpoint). Companion to postJson() -- see its doc comment for shared parameter semantics.
   * Needed because fetchUrl() only supports HTTP Basic auth, not an arbitrary Authorization
   * header value.
   */
  static int getJson(const std::string& url, std::string& outResponse, const std::string& authorization = "",
                     bool verifyTls = false, int* outRetryAfterSeconds = nullptr);
};
