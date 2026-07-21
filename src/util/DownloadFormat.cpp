#include "DownloadFormat.h"

#include <FsHelpers.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

// Case-insensitive substring search; returns the byte offset of the first match, or npos.
size_t findCaseInsensitive(const std::string& haystack, const char* needle) {
  const auto it = std::search(haystack.begin(), haystack.end(), needle, needle + strlen(needle),
                              [](unsigned char a, unsigned char b) { return tolower(a) == tolower(b); });
  return it == haystack.end() ? std::string::npos : static_cast<size_t>(it - haystack.begin());
}

// Percent-decodes a string (RFC 3986 octet decoding, charset-agnostic -- the caller is expected to
// already know the bytes are UTF-8, per RFC 5987 `filename*=UTF-8''...`).
std::string percentDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size() && isxdigit(static_cast<unsigned char>(in[i + 1])) &&
        isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      const char hex[3] = {in[i + 1], in[i + 2], '\0'};
      out += static_cast<char>(strtol(hex, nullptr, 16));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

// Strips CR/LF, reduces to a basename (removes any path separators), and removes ".." sequences.
std::string sanitizeDispositionValue(const std::string& value) {
  std::string cleaned;
  cleaned.reserve(value.size());
  for (const char c : value) {
    if (c != '\r' && c != '\n') cleaned += c;
  }

  const size_t lastSlash = cleaned.find_last_of("/\\");
  if (lastSlash != std::string::npos) cleaned = cleaned.substr(lastSlash + 1);

  size_t dotdot;
  while ((dotdot = cleaned.find("..")) != std::string::npos) {
    cleaned.erase(dotdot, 2);
  }

  return cleaned;
}

// Trims leading/trailing spaces/tabs and, if present, a single pair of surrounding double quotes.
std::string trimAndUnquote(std::string value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

// Case-insensitive ASCII equality (used for the RFC 5987 charset token, e.g. "UTF-8").
bool ciEquals(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; i++) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return i == a.size() && b[i] == '\0';
}

// Extracts the value of a `key=` parameter from a Content-Disposition header value. `key` must
// include the trailing '='. If the value is a quoted-string, reads through to the matching closing
// quote so an embedded ';' (e.g. filename="foo;bar.epub") doesn't truncate it; otherwise reads up to
// the next ';' or the end of the string.
std::string extractParameterValue(const std::string& contentDisposition, const char* key) {
  const size_t pos = findCaseInsensitive(contentDisposition, key);
  if (pos == std::string::npos) return "";
  size_t valueStart = pos + strlen(key);
  while (valueStart < contentDisposition.size() &&
         (contentDisposition[valueStart] == ' ' || contentDisposition[valueStart] == '\t')) {
    valueStart++;
  }
  if (valueStart < contentDisposition.size() && contentDisposition[valueStart] == '"') {
    const size_t closeQuote = contentDisposition.find('"', valueStart + 1);
    if (closeQuote != std::string::npos) {
      return contentDisposition.substr(valueStart, closeQuote - valueStart + 1);
    }
    // No closing quote found; fall through and treat it as an unquoted value.
  }
  const size_t end = contentDisposition.find(';', valueStart);
  return (end == std::string::npos) ? contentDisposition.substr(valueStart)
                                    : contentDisposition.substr(valueStart, end - valueStart);
}

DownloadFormat formatFromExtension(const std::string& name) {
  if (name.empty()) return DownloadFormat::UNKNOWN;
  if (FsHelpers::checkFileExtension(name, ".xtch")) return DownloadFormat::XTCH;
  if (FsHelpers::checkFileExtension(name, ".xtc")) return DownloadFormat::XTC;
  if (FsHelpers::checkFileExtension(name, ".epub")) return DownloadFormat::EPUB;
  return DownloadFormat::UNKNOWN;
}

}  // namespace

namespace DownloadFormatUtils {

const char* extensionForFormat(DownloadFormat format) {
  switch (format) {
    case DownloadFormat::EPUB:
      return ".epub";
    case DownloadFormat::XTC:
      return ".xtc";
    case DownloadFormat::XTCH:
      return ".xtch";
    default:
      return "";
  }
}

std::string parseContentDispositionFilename(const std::string& contentDisposition) {
  if (contentDisposition.empty()) return "";

  // Prefer the RFC 5987 filename* parameter: charset'lang'percent-encoded-value. Only trust it when
  // the charset is UTF-8 (the only encoding percentDecode()'s byte-for-byte decoding is valid for) --
  // otherwise fall through to the plain filename parameter below.
  const std::string starValue = extractParameterValue(contentDisposition, "filename*=");
  if (!starValue.empty()) {
    const size_t firstQuote = starValue.find('\'');
    const size_t secondQuote =
        firstQuote == std::string::npos ? std::string::npos : starValue.find('\'', firstQuote + 1);
    if (secondQuote != std::string::npos && ciEquals(starValue.substr(0, firstQuote), "UTF-8")) {
      const std::string encodedValue = starValue.substr(secondQuote + 1);
      const std::string decoded = sanitizeDispositionValue(percentDecode(trimAndUnquote(encodedValue)));
      if (!decoded.empty()) return decoded;
    }
  }

  // Fall back to the plain filename parameter.
  const std::string plainValue = extractParameterValue(contentDisposition, "filename=");
  if (!plainValue.empty()) {
    const std::string cleaned = sanitizeDispositionValue(trimAndUnquote(plainValue));
    if (!cleaned.empty()) return cleaned;
  }

  return "";
}

DownloadFormat detectFormat(const std::string& dispositionFilename, const std::string& mediaType,
                            const std::string& href) {
  DownloadFormat format = formatFromExtension(dispositionFilename);
  if (format != DownloadFormat::UNKNOWN) return format;

  // Compare only the media type itself, ignoring any trailing parameters (e.g.
  // "application/epub+zip; charset=binary").
  std::string mediaTypeMain = mediaType;
  const size_t paramPos = mediaTypeMain.find(';');
  if (paramPos != std::string::npos) mediaTypeMain.resize(paramPos);
  while (!mediaTypeMain.empty() && (mediaTypeMain.back() == ' ' || mediaTypeMain.back() == '\t')) {
    mediaTypeMain.pop_back();
  }

  if (mediaTypeMain == "application/epub+zip") return DownloadFormat::EPUB;
  if (mediaTypeMain == "application/vnd.xteink.xtc") return DownloadFormat::XTC;
  // application/octet-stream is intentionally not used as a signal here.

  std::string path = href;
  const size_t cutPos = path.find_first_of("?#");
  if (cutPos != std::string::npos) path.resize(cutPos);

  return formatFromExtension(path);
}

std::string stripExtension(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return name;
  return name.substr(0, dot);
}

}  // namespace DownloadFormatUtils
