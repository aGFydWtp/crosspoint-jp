#pragma once

#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

/**
 * Format a byte count as a human-readable size string.
 * Uses "%.1f MB" for sizes >= 1 MiB, "%.0f KB" for sizes >= 1 KiB, otherwise "%zu B".
 */
std::string formatSize(size_t bytes);

}  // namespace StringUtils
