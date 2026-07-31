#!/usr/bin/env python3
"""
Pre-build check: verify that the built-in CJK UI font header (cjk_ui_font_20.h)
covers everything it is supposed to cover.

Two sources are checked:

1. lib/I18n/translations/*.yaml - every CJK character used by the UI strings.
2. scripts/codepoints_*.txt     - the curated code point lists that were fed to
   the generator (JIS levels, CP932 extensions, UI symbols, and the baseline
   snapshot of previously shipped glyphs).

(2) is what catches "a symbol silently went missing". Book titles, author names
and file names are dynamic, so they can never be validated from the sources -
pinning the intended coverage in the codepoints files is the only way to notice
a regression at build time.

If missing characters are found, the build fails with an actionable error message.

Usage (standalone):
    python3 check_cjk_ui_font.py

Usage (PlatformIO pre-build):
    Added automatically via platformio.ini extra_scripts.
"""

import glob
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("Warning: PyYAML not installed, skipping CJK UI font check")
    sys.exit(0)


def extract_cjk_from_translations(translations_dir):
    """Extract all CJK characters (U+3000+) from translation YAML files."""
    chars = set()
    for path in sorted(glob.glob(str(Path(translations_dir) / "*.yaml"))):
        with open(path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
        for key, value in data.items():
            if key.startswith("_"):
                continue
            for c in str(value):
                if ord(c) >= 0x3000:
                    chars.add(c)
    return chars


def extract_codepoints_from_lists(scripts_dir):
    """Extract required code points from every scripts/codepoints_*.txt file."""
    required = {}
    for path in sorted(glob.glob(str(Path(scripts_dir) / "codepoints_*.txt"))):
        name = Path(path).name
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    required.setdefault(int(line, 16), name)
                except ValueError:
                    pass
    return required


def extract_codepoints_from_header(header_path):
    """Extract codepoints from the CJK_UI_CODEPOINTS array in the header file."""
    with open(header_path, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"CJK_UI_CODEPOINTS\[\] PROGMEM = \{(.*?)\};", content, re.S)
    if not match:
        return set()
    return {int(cp, 16) for cp in re.findall(r"0x([0-9A-Fa-f]{4})", match.group(1))}


def _report(missing, source_label):
    print(f"\n*** CJK UI Font Check Failed ***")
    print(f"{len(missing)} characters required by {source_label} are missing from cjk_ui_font_20.h:\n")
    preview = missing[:80]
    print("  " + "".join(chr(cp) for cp in preview) + (" ..." if len(missing) > len(preview) else ""))
    print("  " + " ".join(f"U+{cp:04X}" for cp in preview) + (" ..." if len(missing) > len(preview) else ""))
    print("\nRegenerate the font - see docs/cjk-fonts.md for the full command line.")
    print()


def check(project_root):
    translations_dir = project_root / "lib" / "I18n" / "translations"
    scripts_dir = project_root / "scripts"
    header_path = project_root / "lib" / "GfxRenderer" / "cjk_ui_font_20.h"

    if not header_path.exists():
        return True

    header_codepoints = extract_codepoints_from_header(header_path)
    if not header_codepoints:
        print("*** CJK UI Font Check Failed ***")
        print(f"Could not parse CJK_UI_CODEPOINTS from {header_path}")
        return False

    ok = True

    required = extract_codepoints_from_lists(scripts_dir)
    if required:
        missing = sorted(cp for cp in required if cp not in header_codepoints)
        if missing:
            by_list = {}
            for cp in missing:
                by_list.setdefault(required[cp], []).append(cp)
            _report(missing, "scripts/codepoints_*.txt")
            for name, cps in sorted(by_list.items()):
                print(f"  {name}: {len(cps)} missing")
            print()
            ok = False

    if translations_dir.is_dir():
        translation_chars = extract_cjk_from_translations(translations_dir)
        missing = sorted(ord(c) for c in translation_chars if ord(c) not in header_codepoints)
        if missing:
            _report(missing, "lib/I18n/translations/*.yaml")
            ok = False

    if ok:
        print(f"CJK UI font check: OK ({len(header_codepoints)} glyphs, {len(required)} pinned code points)")
    return ok


def main():
    project_root = Path(__file__).parent.parent
    if not check(project_root):
        sys.exit(1)


if __name__ == "__main__":
    main()
else:
    try:
        Import("env")
        _project_root = Path(env.subst("$PROJECT_DIR"))
        if not check(_project_root):
            Import("env")
            env.Exit(1)
    except NameError:
        pass
