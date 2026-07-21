#pragma once
#include <NetworkClientSecure.h>

/**
 * NetworkClientSecure subclass that exposes enabling the framework's built-in
 * CA certificate bundle (esp_crt_bundle_attach) for full chain + hostname
 * verification.
 *
 * Background: pioarduino's prebuilt esp32c3 libraries already embed the full
 * Mozilla CA bundle (~67KB) used by esp_crt_bundle_attach -- ssl_client.cpp.o
 * references that symbol and it is already linked into firmware.bin, so
 * enabling it here costs no additional flash.
 *
 * NetworkClientSecure::setCACertBundle(NULL, 0) cannot be used to enable the
 * *default* bundle: passing a null buffer is documented as disabling bundle
 * verification, not selecting the default. Enabling the default bundle
 * requires calling attach_ssl_certificate_bundle()/_use_ca_bundle directly,
 * both of which are protected members only reachable from a subclass.
 */
class SecureNetworkClient : public NetworkClientSecure {
 public:
  /// Enables chain + hostname verification against the framework's embedded default CA bundle.
  void useDefaultCertBundle() {
    attach_ssl_certificate_bundle(sslclient.get(), true);
    _use_ca_bundle = true;
  }
};
