#pragma once

class GfxRenderer;

/**
 * Release every font cache that can be rebuilt on demand, right before a
 * network activity starts making HTTPS requests.
 *
 * A TLS handshake on this device needs a ~16.5KB contiguous block for the
 * inbound record buffer (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN cannot go lower:
 * release-assets.githubusercontent.com was measured sending full 16,408-byte
 * records) plus ~4KB for the outbound one, and then a few KB more in small
 * chunks while mbedtls parses the peer chain and verifies the signature against
 * the CA bundle. Starving that last stage surfaces as tls=12288
 * (-MBEDTLS_ERR_X509_FATAL_ERROR), which reads like a certificate problem but
 * is really out-of-memory inside esp_crt_check_signature().
 *
 * Everything freed here is lazy-loaded again on next use, and font IDs stay
 * registered, so section caches keyed on them remain valid.
 *
 * @param tag  log tag of the calling activity, e.g. "FONT" or "AOZORA"
 */
void reclaimHeapForTls(GfxRenderer& renderer, const char* tag);
