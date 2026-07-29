#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

namespace {
// One acquisition link per supported format (EPUB / XTC / XTCH). A feed that lists several links
// for the same format keeps the first one.
constexpr size_t MAX_ACQUISITION_LINKS = 3;
// Per-link and aggregate href budgets. Before multi-format support each entry stored exactly one
// href, so the aggregate cap is set to the worst case that was already reachable for a full feed
// page (64 entries x 768 chars): holding up to three links per entry must not raise the parser's
// peak DRAM footprint above what it already was.
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_ACQUISITION_HREF_CHARS = 64 * MAX_HREF_CHARS;

// Returns the bare media type of a `type` attribute, i.e. with any parameters ("; charset=...")
// and surrounding whitespace removed. Views into `type`; valid only while `type` is.
std::string_view mimeTypeToken(const char* type) {
  if (!type) return {};

  std::string_view token{type};
  if (const size_t parameters = token.find(';'); parameters != std::string_view::npos) {
    token = token.substr(0, parameters);
  }
  while (!token.empty() && isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
  while (!token.empty() && isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
  return token;
}

bool equalsIgnoreCase(const std::string_view value, const std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (size_t i = 0; i < value.size(); i++) {
    if (tolower(static_cast<unsigned char>(value[i])) != tolower(static_cast<unsigned char>(expected[i]))) return false;
  }
  return true;
}

// MIME types are case-insensitive per RFC 2045, and XTEink ships both a vendor tree
// (application/vnd.xteink.*) and an experimental +zip form, so both spellings are accepted.
OpdsAcquisitionFormat formatFromMimeType(const std::string_view type) {
  if (equalsIgnoreCase(type, "application/epub+zip")) return OpdsAcquisitionFormat::EPUB;
  if (equalsIgnoreCase(type, "application/vnd.xteink.xtc") || equalsIgnoreCase(type, "application/x-xtc+zip")) {
    return OpdsAcquisitionFormat::XTC;
  }
  if (equalsIgnoreCase(type, "application/vnd.xteink.xtch") || equalsIgnoreCase(type, "application/x-xtch+zip")) {
    return OpdsAcquisitionFormat::XTCH;
  }
  return OpdsAcquisitionFormat::UNKNOWN;
}

bool hasSuffixIgnoreCase(const std::string_view value, const std::string_view suffix) {
  return value.size() >= suffix.size() && equalsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

OpdsAcquisitionFormat formatFromUrl(const char* href) {
  std::string_view path{href ? href : ""};
  if (const size_t suffix = path.find_first_of("?#"); suffix != std::string_view::npos) {
    path = path.substr(0, suffix);
  }
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  if (hasSuffixIgnoreCase(path, ".xtch")) return OpdsAcquisitionFormat::XTCH;
  if (hasSuffixIgnoreCase(path, ".xtc")) return OpdsAcquisitionFormat::XTC;
  if (hasSuffixIgnoreCase(path, ".epub")) return OpdsAcquisitionFormat::EPUB;
  return OpdsAcquisitionFormat::UNKNOWN;
}

// Resolves the format a link advertises. A specific MIME type always wins; the URL extension is
// consulted only when the feed gave no type or a generic one, since many servers (html2xtc among
// them) fall back to application/octet-stream for every format they serve.
OpdsAcquisitionFormat acquisitionFormat(const char* type, const char* href) {
  const std::string_view mimeType = mimeTypeToken(type);
  if (const auto format = formatFromMimeType(mimeType); format != OpdsAcquisitionFormat::UNKNOWN) return format;

  const bool genericMime = mimeType.empty() || equalsIgnoreCase(mimeType, "application/octet-stream") ||
                           equalsIgnoreCase(mimeType, "binary/octet-stream");
  return genericMime ? formatFromUrl(href) : OpdsAcquisitionFormat::UNKNOWN;
}
}  // namespace

const char* opdsAcquisitionLabel(const OpdsAcquisitionFormat format) {
  switch (format) {
    case OpdsAcquisitionFormat::EPUB:
      return "EPUB";
    case OpdsAcquisitionFormat::XTC:
      return "XTC";
    case OpdsAcquisitionFormat::XTCH:
      return "XTCH";
    case OpdsAcquisitionFormat::UNKNOWN:
    default:
      return "";
  }
}

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    void* const buf = XML_GetBuffer(parser, chunkSize);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  acquisitionHrefChars = 0;
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
}

void OpdsParser::addAcquisitionLink(const char* type, const char* href) {
  const auto format = acquisitionFormat(type, href);
  if (format == OpdsAcquisitionFormat::UNKNOWN) return;

  auto& links = currentEntry.acquisitionLinks;
  for (const auto& link : links) {
    if (link.format == format) return;  // First link of a given format wins
  }
  if (links.size() >= MAX_ACQUISITION_LINKS) return;

  const size_t hrefChars = strnlen(href, MAX_HREF_CHARS + 1);
  if (hrefChars > MAX_HREF_CHARS || hrefChars > MAX_ACQUISITION_HREF_CHARS - acquisitionHrefChars) {
    LOG_DBG("OPDS", "Dropping acquisition link: href budget exceeded (%zu chars)", hrefChars);
    return;
  }

  // No reserve() here on purpose: the vector holds at most MAX_ACQUISITION_LINKS elements and the
  // overwhelmingly common case is a single link, so reserving up front would waste ~70 bytes on
  // every book entry to avoid at most two small growths on the rare multi-format entry.
  OpdsAcquisitionLink link;
  link.href.assign(href, hrefChars);
  link.format = format;
  acquisitionHrefChars += hrefChars;
  links.push_back(std::move(link));

  currentEntry.type = OpdsEntryType::BOOK;
  // Books address their downloads through acquisitionLinks; drop any navigation href picked up
  // from an earlier atom+xml link on the same entry so the two can't disagree.
  currentEntry.href.clear();
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href && href[0] != '\0') {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          self->addAcquisitionLink(type, href);
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    // Books are addressable through acquisitionLinks, navigation entries through href.
    const bool hasTarget = self->currentEntry.type == OpdsEntryType::BOOK ? !self->currentEntry.acquisitionLinks.empty()
                                                                          : !self->currentEntry.href.empty();
    if (!self->currentEntry.title.empty() && hasTarget) {
      self->entries.push_back(std::move(self->currentEntry));
    } else {
      // Return the discarded entry's hrefs to the aggregate budget so a feed full of unusable
      // entries can't starve the entries that follow it.
      for (const auto& link : self->currentEntry.acquisitionLinks) {
        self->acquisitionHrefChars -= link.href.size();
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  }
}
