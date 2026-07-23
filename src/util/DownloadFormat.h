#pragma once

#include <string>

/**
 * Detected on-disk format for an OPDS acquisition-link download.
 */
enum class DownloadFormat { EPUB, XTC, XTCH, UNKNOWN };

/**
 * Helpers for classifying an OPDS book download and deriving a safe destination filename from
 * HTTP response metadata (Content-Disposition, Content-Type) and the OPDS entry itself.
 */
namespace DownloadFormatUtils {

/// Canonical extension (including the leading dot) for a format, or "" for UNKNOWN.
const char* extensionForFormat(DownloadFormat format);

/**
 * Parses the filename suggested by a Content-Disposition header value, e.g.
 * `attachment; filename="fallback.xtc"; filename*=UTF-8''%E4%BD%9C%E5%93%81.xtc`.
 * Prefers the RFC 5987 `filename*` parameter (percent-decoded) over the plain `filename` parameter.
 *
 * The returned name has CR/LF stripped, is reduced to a basename (any path separators removed), and
 * has ".." sequences removed. It is NOT yet FAT32-sanitized -- pass it through
 * StringUtils::sanitizeFilename() before using it as an actual filename.
 *
 * @return The decoded filename, or "" if the header is empty/absent or no filename could be parsed.
 */
std::string parseContentDispositionFilename(const std::string& contentDisposition);

/**
 * Determines the download format using, in priority order:
 *   1. The extension of `dispositionFilename` (from parseContentDispositionFilename()).
 *   2. `mediaType` (application/epub+zip -> EPUB, application/vnd.xteink.xtc -> XTC；
 *      application/octet-stream is deliberately not treated as a signal here, since html2xtc uses it
 *      as a generic fallback content type for both EPUB and XTC).
 *   3. The extension of `href` (query string stripped first).
 * Returns UNKNOWN if none of the above yields a recognized extension.
 */
DownloadFormat detectFormat(const std::string& dispositionFilename, const std::string& mediaType,
                            const std::string& href);

/// Strips a trailing ".ext" from `name` if present (used to avoid double-extension bugs when the
/// canonical extension for the detected format is appended afterwards).
std::string stripExtension(const std::string& name);

}  // namespace DownloadFormatUtils
