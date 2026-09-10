#!/bin/sh

# SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

launcher="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/heaptrack"
client="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/tst_macos_preload_client"
printer="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/heaptrack_print"
output_base="@CMAKE_CURRENT_BINARY_DIR@/tst macos e2e"
report="$output_base.txt"

rm -f "$output_base.gz" "$output_base.zst" "$report"
trap 'rm -f "$output_base.gz" "$output_base.zst" "$report"' EXIT

"$launcher" --record-only --quiet --output "$output_base" "$client" > /dev/null
if test -f "$output_base.zst"; then
    data="$output_base.zst"
else
    data="$output_base.gz"
fi
test -s "$data"

"$printer" "$data" > "$report"
grep -F 'calls to allocation functions: 7 (' "$report"
grep -Fx 'main' "$report"
grep -F 'tst_macos_preload_client.c:' "$report"
