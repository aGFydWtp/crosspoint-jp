"""
PlatformIO pre-build script: neutralize PNGdec's ESP32-S3 SIMD assembly.

Problem:
  PNGdec ships src/s3_simd_rgb565.S, whose only guard is `#ifdef
  ARDUINO_ARCH_ESP32` — true for every ESP32 variant, not just the S3 it was
  written for. Line 9 then includes dsps_fft2r_platform.h, a header owned by
  the espressif/esp-dsp managed component.

  On the prebuilt-libs path that header happens to be on the include path, so
  the file assembles into nothing (the body is behind
  `#if (dsps_fft2r_sc16_aes3_enabled == 1)`, false on RISC-V) and nobody
  notices. Once custom_sdkconfig switches the build to ESP-IDF from source,
  the include resolution changes and the assembler fails outright:

    s3_simd_rgb565.S:9:10: fatal error: dsps_fft2r_platform.h: No such file

Fix:
  Widen the guard to require the S3 target. The SIMD code is Xtensa-only, so
  on this ESP32-C3 firmware the whole file is dead weight either way.

Applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        simd_asm = os.path.join(libdeps_dir, env_dir, "PNGdec", "src", "s3_simd_rgb565.S")
        if os.path.isfile(simd_asm):
            _restrict_simd_to_s3(simd_asm)


def _restrict_simd_to_s3(filepath):
    MARKER = "// CrossPoint patch: S3-only guard"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = "#ifdef ARDUINO_ARCH_ESP32"
    NEW = MARKER + "\n#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S3)"

    if OLD not in content:
        print(
            "WARNING: PNGdec S3 SIMD guard patch target not found in %s "
            "— library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched PNGdec: restricted s3_simd_rgb565.S to ESP32-S3: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_pngdec(env)
