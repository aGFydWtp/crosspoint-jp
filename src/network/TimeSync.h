#pragma once

/**
 * Small helper for making sure the device clock is set before code that depends on wall-clock
 * time runs (in Phase 1, TLS certificate validity checking: a not-yet-synced clock reads as
 * 1970 and any real certificate's notBefore check fails).
 *
 * X4/X3 have no RTC battery-backed across power loss, so time(nullptr) can read near the epoch
 * after a cold boot until NTP sync completes.
 */
class TimeSync {
 public:
  /// True if the system clock already holds a plausible (post 2024-01-01 UTC) value.
  static bool isTimeValid();

  /**
   * Ensures the system clock holds a plausible value, synchronizing via NTP if it doesn't
   * already. Requires an active network connection (Wi-Fi) -- callers must connect first.
   *
   * Blocking: polls time(nullptr) every 100ms until valid or timeoutMs elapses. Safe to call
   * from the main activity loop the same way HttpDownloader calls already block it.
   *
   * @param timeoutMs Maximum time to wait for NTP sync to complete.
   * @return true if the clock is valid (either already, or after a successful sync) before the
   *         timeout elapsed; false if it timed out.
   */
  static bool ensureSynced(int timeoutMs);
};
