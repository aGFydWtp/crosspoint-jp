#include "TlsHeapReclaim.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

void reclaimHeapForTls(GfxRenderer& renderer, const char* tag) {
  const int heapBefore = static_cast<int>(ESP.getFreeHeap());
  const int blkBefore = static_cast<int>(ESP.getMaxAllocHeap());

  // ExternalFont LRU caches (~34KB each).
  FontManager& fm = FontManager::getInstance();
  ExternalFont* uiFont = fm.getActiveUiFont();
  ExternalFont* readerFont = fm.getActiveFont();
  if (uiFont) uiFont->unload();
  if (readerFont) readerFont->unload();

  // SD card font prewarm data and kern/ligature tables, plus the compressed
  // builtin fonts' decompressor cache. The reader releases these on exit, but
  // anything drawn since then can have pulled them back in — and the OTA/OPDS
  // entry points do not go through the reader at all.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  LOG_DBG(tag, "Reclaimed for TLS: heap %d->%d, blk %d->%d", heapBefore, static_cast<int>(ESP.getFreeHeap()), blkBefore,
          static_cast<int>(ESP.getMaxAllocHeap()));
}
