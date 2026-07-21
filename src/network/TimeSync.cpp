#include "TimeSync.h"

#include <Arduino.h>
#include <Logging.h>

#include <ctime>

namespace {
// 2024-01-01 00:00:00 UTC. Before NTP sync, time() reads near the epoch (1970), so any
// timestamp at or after this threshold is a reasonable "the clock has been set" signal.
constexpr time_t kValidTimeThreshold = 1704067200;
}  // namespace

bool TimeSync::isTimeValid() { return time(nullptr) >= kValidTimeThreshold; }

bool TimeSync::ensureSynced(int timeoutMs) {
  if (isTimeValid()) {
    return true;
  }

  LOG_DBG("TIME", "Clock not set, starting NTP sync");

  // configTime() internally calls setTimeZone(), which overwrites the process TZ env var --
  // clobbering the JST-9 setting main.cpp applies at boot (src/main.cpp:312-313). NTP sync
  // itself only sets the UTC epoch via settimeofday(); it doesn't need or use TZ. Restore JST-9
  // immediately after so localtime_r() elsewhere in the app keeps returning Japan time.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", "JST-9", 1);
  tzset();

  constexpr int kPollIntervalMs = 100;
  int waitedMs = 0;
  while (!isTimeValid() && waitedMs < timeoutMs) {
    delay(kPollIntervalMs);
    waitedMs += kPollIntervalMs;
  }

  const bool synced = isTimeValid();
  LOG_DBG("TIME", "NTP sync %s after %dms", synced ? "succeeded" : "timed out", waitedMs);
  return synced;
}
