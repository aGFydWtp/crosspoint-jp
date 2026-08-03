#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <cstring>
#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/aGFydWtp/crosspoint-jp/releases/latest";

// esp_err_to_name の "ESP_ERR_" prefix を削って画面幅に収まる短い名前にする。
const char* shortErrName(const char* name) {
  if (!name) return "?";
  if (strncmp(name, "ESP_ERR_", 8) == 0) return name + 8;
  return name;
}

bool parseSemver3(const char* version, int* major, int* minor, int* patch) {
  if (!version || !major || !minor || !patch) {
    return false;
  }
  const char* p = version;
  if (*p == 'v' || *p == 'V') {
    p++;
  }
  return sscanf(p, "%d.%d.%d", major, minor, patch) == 3;
}

/*
 * Returns the fork release sequence encoded in versions like "0.1.11-forked.3".
 * parseSemver3 only reads three numeric segments, so without this the whole
 * "-forked.N" suffix is discarded and consecutive fork releases of the same
 * base version compare as equal.
 *
 * The numbering mirrors .github/workflows/release-dispatch.yml: a bare
 * "-forked" is sequence 0 and "-forked.N" is N. A version with no "-forked"
 * suffix returns -1 so that any fork release sorts after it.
 */
int parseForkedSeq(const char* version) {
  if (!version) {
    return -1;
  }
  const char* suffix = strstr(version, "-forked");
  if (!suffix) {
    return -1;
  }
  suffix += sizeof("-forked") - 1;
  if (*suffix != '.') {
    return 0;
  }
  int seq;
  return sscanf(suffix + 1, "%d", &seq) == 1 ? seq : 0;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  lastErrorDetail.clear();

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    char buf[64];
    snprintf(buf, sizeof(buf), "fetch fail http=%d heap=%dKB", HttpDownloader::lastHttpCode,
             static_cast<int>(ESP.getFreeHeap() / 1024));
    lastErrorDetail = buf;
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  if (!parseSemver3(latestVersion.c_str(), &latestMajor, &latestMinor, &latestPatch) ||
      !parseSemver3(currentVersion, &currentMajor, &currentMinor, &currentPatch)) {
    LOG_ERR("OTA", "Version parse failed: current=%s, latest=%s", currentVersion, latestVersion.c_str());
    return false;
  }

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  /*
   * Same major.minor.patch. This fork republishes upstream base versions as
   * vX.Y.Z-forked.N, so compare that sequence too; otherwise every fork release
   * of an unchanged base version looks identical to the installed build and is
   * never offered over OTA.
   */
  const int latestForked = parseForkedSeq(latestVersion.c_str());
  const int currentForked = parseForkedSeq(currentVersion);
  if (latestForked != currentForked) return latestForked > currentForked;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  lastErrorDetail.clear();

  // esp_https_ota opens its own esp_http_client with its own buffer sizes and
  // no visibility into the TLS diagnostics this fork captures. Drive the OTA
  // partition ourselves and stream the firmware through HttpDownloader instead,
  // reusing its redirect handling for the GitHub -> CDN hop and the LARGE TX
  // profile that hop's signed URL needs.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    lastErrorDetail = "no ota partition";
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    char buf[96];
    snprintf(buf, sizeof(buf), "begin:%s heap=%dKB", shortErrName(esp_err_to_name(esp_err)),
             static_cast<int>(ESP.getFreeHeap() / 1024));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  esp_err_t writeErr = ESP_OK;
  // LARGE buffers: otaUrl points at the release asset, which redirects to a
  // signed release-assets.githubusercontent.com URL whose request line does not
  // fit the default 512-byte TX buffer. FontDownloadActivity needs them for the
  // same reason — see HttpDownloader::BufferProfile.
  const bool fetchOk = HttpDownloader::fetchUrl(
      otaUrl,
      [&](const uint8_t* data, size_t len) {
        writeErr = esp_ota_write(otaHandle, data, len);
        if (writeErr != ESP_OK) {
          flashOk = false;
          return false;  // abort the transfer
        }
        processedSize += len;
        // Fire the callback only on whole-percent change. Per-chunk updates wake the
        // render task, whose framebuffer work contends with TLS on the internal arena,
        // and e-ink can't repaint faster than a percent tick anyway.
        if (onProgress && totalSize > 0) {
          const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
          if (pct != lastReportedPct) {
            lastReportedPct = pct;
            onProgress(ctx);
          }
        }
        return true;
      },
      "", "", HttpDownloader::BufferProfile::LARGE);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    char buf[128];
    if (!flashOk) {
      snprintf(buf, sizeof(buf), "write:%s r=%d/%d|heap=%dKB blk=%dKB", shortErrName(esp_err_to_name(writeErr)),
               static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
               static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    } else {
      snprintf(buf, sizeof(buf), "fetch http=%d r=%d/%d|heap=%dKB blk=%dKB", HttpDownloader::lastHttpCode,
               static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
               static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    }
    lastErrorDetail = buf;
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    char buf[128];
    snprintf(buf, sizeof(buf), "end:%s r=%d/%d|heap=%dKB blk=%dKB", shortErrName(esp_err_to_name(esp_err)),
             static_cast<int>(processedSize), static_cast<int>(totalSize), static_cast<int>(ESP.getFreeHeap() / 1024),
             static_cast<int>(ESP.getMaxAllocHeap() / 1024));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    char buf[96];
    snprintf(buf, sizeof(buf), "setboot:%s", shortErrName(esp_err_to_name(esp_err)));
    lastErrorDetail = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
