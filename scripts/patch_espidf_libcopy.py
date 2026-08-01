"""
PlatformIO pre-build script: make pioarduino copy the nested mbedTLS archives.

Problem:
  With custom_sdkconfig set, pioarduino builds ESP-IDF from source and then
  copies the resulting archives back into the framework-arduinoespressif32-libs
  package, which is what actually gets linked. That copy step
  (builder/frameworks/espidf.py, idf_lib_copy) walks exactly one level:

      for folder in src:                       # esp-idf/<component>/
          files = [... for x in os.listdir(folder)]
          for file in files:
              if file.strip().endswith(".a"):
                  shutil.copyfile(...)

  The mbedTLS component's own wrapper archive sits at that level and is copied.
  Upstream mbedTLS builds one level deeper — esp-idf/mbedtls/mbedtls/library/
  and .../3rdparty/ — so libmbedtls_2.a (the upstream core, holding ssl_tls.c
  and ssl_msg.c), libmbedcrypto.a, libmbedx509.a, libeverest.a and libp256m.a
  are never copied. The package keeps whatever Espressif shipped.

  The result is silent and expensive: every mbedTLS option in custom_sdkconfig
  is compiled into archives that get thrown away. On this firmware that meant
  CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN never took effect, so instead of the
  intended 16KB in / 2KB out the device kept Espressif's symmetric 16KB/16KB —
  ~33KB of record buffers per connection instead of ~18.5KB. That 14KB is the
  difference between a TLS handshake that completes and one that dies with
  MBEDTLS_ERR_RSA_PUBLIC_FAILED + MBEDTLS_ERR_MPI_ALLOC_FAILED while verifying
  the server's root certificate.

  Diagnosing it from the outside is nasty, because the sdkconfig the copy step
  leaves in the package *does* show the requested values — it is written from
  the project's sdkconfig, not from what the surviving archives were built
  with. The honest record is sdkconfig.orig, and the archive timestamps: the
  component wrappers are current while the upstream cores stay at the package's
  release date.

Fix:
  After the existing one-level loop, copy every deeper archive too, renaming
  the upstream libmbedtls.a to libmbedtls_2.a because that is the name
  flags/ld_libs links against (-lmbedtls for the wrapper, -lmbedtls_2 for the
  core).

  Already fixed upstream, but only in 55.03.311 (2026-07-24), which also jumps
  Arduino 3.3.7 -> 3.3.11 and ESP-IDF 5.5.2 -> 5.5.5. 55.03.38/38-1/39 do not
  carry it. Until this project takes that upgrade, patch locally.

Applied idempotently — safe to run on every build. Rewrites espidf.py only when
the marker is absent, and does nothing at all on a platform that already copies
nested archives itself (detected via copy_idf_component_archives), so bumping
the platform later needs no change here.

Note that espidf.py lives in the shared PlatformIO platform directory, so this
also changes idf_lib_copy for every other project on the machine. That is a
strict improvement — those projects were losing the same archives — but it is a
side effect worth knowing about.
"""

Import("env")

import os

MARKER = "# CrossPoint patch: copy nested upstream archives"

ANCHOR = """        for folder in src:
            files = [str(Path(folder) / x) for x in os.listdir(folder)]
            for file in files:
                if file.strip().endswith(".a"):
                    shutil.copyfile(file, str(Path(lib_dst) / file.split(os.path.sep)[-1]))
"""

INSERT = """
        {marker}: the loop above only reaches esp-idf/<component>/*.a, but
        # upstream mbedTLS builds into esp-idf/mbedtls/mbedtls/library/ and
        # .../3rdparty/. Without these, every mbedTLS setting in
        # custom_sdkconfig is compiled and then discarded.
        # Scoped to mbedtls on purpose. A blanket rglob over lib_src would also
        # pick up any other component's nested archives, and since only
        # libmbedtls.a gets renamed here, a basename colliding with a top-level
        # archive would be overwritten without a word. Upstream's fix renames
        # collisions generically (_2/_3/...); this one only claims to fix the
        # case it was written for.
        for nested in (Path(lib_src) / "mbedtls").rglob("*.a"):
            if nested.parent == Path(lib_src) / "mbedtls":
                continue  # the component wrapper, already handled one level up
            # ld_libs expects -lmbedtls for the component wrapper and
            # -lmbedtls_2 for the upstream core; both are named libmbedtls.a.
            name = "libmbedtls_2.a" if nested.name == "libmbedtls.a" else nested.name
            shutil.copyfile(str(nested), str(Path(lib_dst) / name))
""".format(marker=MARKER)


def _platform_script(env):
    platform_dir = env.PioPlatform().get_dir()
    return os.path.join(platform_dir, "builder", "frameworks", "espidf.py")


def patch_libcopy(env):
    path = _platform_script(env)
    if not os.path.isfile(path):
        print("patch_espidf_libcopy: espidf.py not found, skipping (%s)" % path)
        return

    with open(path, "r", encoding="utf-8") as fh:
        content = fh.read()

    if MARKER in content:
        return

    if "copy_idf_component_archives" in content:
        # Platform >= 55.03.311 walks the component directories recursively on
        # its own. Nothing to do, and nothing to warn about.
        return

    if ANCHOR not in content:
        # Platform layout changed. Say so loudly rather than silently shipping a
        # firmware whose TLS buffers are not what platformio.ini asks for.
        print("*** patch_espidf_libcopy: neither the one-level copy loop nor the "
              "upstream recursive copy was found in espidf.py — mbedTLS options "
              "in custom_sdkconfig may NOT take effect. Re-check idf_lib_copy "
              "against this platform version. ***")
        return

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content.replace(ANCHOR, ANCHOR + INSERT, 1))

    print("*** patch_espidf_libcopy: espidf.py patched to copy nested mbedTLS archives ***")


patch_libcopy(env)
