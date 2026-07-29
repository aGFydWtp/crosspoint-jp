#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Acquisition format declared by an OPDS feed for a book's download link.
 *
 * This records only what the *feed* claims. The format a download is actually saved as is
 * resolved from the HTTP response at download time (see DownloadFormat in src/util/), which is
 * more reliable; this enum is the last-resort fallback and drives the format-selection UI when a
 * single entry offers more than one format.
 */
enum class OpdsAcquisitionFormat : uint8_t { UNKNOWN, EPUB, XTC, XTCH };

/**
 * A single `rel="...opds-spec.org/acquisition"` link belonging to a book entry.
 */
struct OpdsAcquisitionLink {
  std::string href;
  OpdsAcquisitionFormat format = OpdsAcquisitionFormat::UNKNOWN;
};

/// Short label for a format ("EPUB", "XTC", "XTCH"), or "" for UNKNOWN. Deliberately not run
/// through tr(): these are file format names and are identical in every language.
const char* opdsAcquisitionLabel(OpdsAcquisitionFormat format);

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL only; books carry their download URLs in acquisitionLinks
  std::string id;
  // Books only: at most one link per supported format, in feed order.
  std::vector<OpdsAcquisitionLink> acquisitionLinks;
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);

  // Records an acquisition link on currentEntry if its format is recognized, not already present,
  // and still within the link-count and aggregate href budgets. Silently ignored otherwise.
  void addAcquisitionLink(const char* type, const char* href);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;
  // Total href bytes held across every stored acquisition link, used to bound the parser's peak
  // DRAM footprint now that one entry can hold several links.
  size_t acquisitionHrefChars = 0;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;

  bool errorOccured = false;
};
