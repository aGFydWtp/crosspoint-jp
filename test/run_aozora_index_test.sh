#!/usr/bin/env bash
set -euo pipefail

# 青空文庫ダウンロード履歴のバイナリ形式契約テストをホスト側で実行する。
# AozoraIndexManager 本体は SD / ArduinoJson 依存でホストではリンクできないため、
# ヘッダの契約（struct レイアウト、定数、境界計算）のみを検証する。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/aozora_index"
BINARY="$BUILD_DIR/AozoraIndexTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/aozora_index/AozoraIndexTest.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
