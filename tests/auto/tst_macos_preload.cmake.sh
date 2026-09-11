#!/bin/sh

# SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

raw_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload.raw"
pointer_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload.pointers"
preload_library="@PROJECT_BINARY_DIR@/@LIB_INSTALL_DIR@/heaptrack/libheaptrack_preload@CMAKE_SHARED_LIBRARY_SUFFIX@"
client="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/tst_macos_preload_client"

ready_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload.ready"
rm -f "$ready_file"
: > "$ready_file"
DYLD_INSERT_LIBRARIES="$preload_library" DUMP_HEAPTRACK_OUTPUT="/dev/null/heaptrack.raw" \
    DUMP_HEAPTRACK_READY="$ready_file" "$client" > /dev/null 2>&1
test ! -s "$ready_file"

rm -f "$raw_file" "$pointer_file"
DYLD_INSERT_LIBRARIES="$preload_library" DUMP_HEAPTRACK_OUTPUT="$raw_file" "$client" > "$pointer_file"

check_record() {
    allocator="$1"
    size="$2"
    pointer=$(sed -n "s/^${allocator}=0x//p" "$pointer_file")
    test -n "$pointer"
    grep -Eq "^\\+ ${size} [0-9a-f]+ ${pointer}$" "$raw_file"
    grep -Eq "^- ${pointer}$" "$raw_file"
}

check_record malloc 12341
check_record calloc 12342
check_record realloc 12343
check_record valloc 12344
check_record posix_memalign 12345
check_record aligned_alloc 12350

stop_raw_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload_stop.raw"
rm -f "$stop_raw_file"
DYLD_INSERT_LIBRARIES="$preload_library" DUMP_HEAPTRACK_OUTPUT="$stop_raw_file" \
    "$client" --explicit-stop > /dev/null
test -s "$stop_raw_file"

edge_raw_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload_edge.raw"
edge_pointer_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload_edge.pointers"
rm -f "$edge_raw_file" "$edge_pointer_file"
DYLD_INSERT_LIBRARIES="$preload_library" DUMP_HEAPTRACK_OUTPUT="$edge_raw_file" \
    "$client" --edge-cases > "$edge_pointer_file"
failed_realloc_pointer=$(sed -n 's/^failed_realloc=0x//p' "$edge_pointer_file")
test -n "$failed_realloc_pointer"
test "$(grep -Ec "^- ${failed_realloc_pointer}$" "$edge_raw_file")" -eq 1
grep -Eq "^\\+ 23456 [0-9a-f]+ ${failed_realloc_pointer}$" "$edge_raw_file"
test "$(grep -c '^+ ' "$edge_raw_file")" -eq 2
test "$(grep -c '^- ' "$edge_raw_file")" -eq 2

darwin_raw_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload_darwin.raw"
darwin_pointer_file="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_preload_darwin.pointers"
rm -f "$darwin_raw_file" "$darwin_pointer_file"
DYLD_INSERT_LIBRARIES="$preload_library" DUMP_HEAPTRACK_OUTPUT="$darwin_raw_file" \
    "$client" --darwin-allocators > "$darwin_pointer_file"

while IFS='=:' read -r allocator size pointer; do
    test -n "$allocator" && test -n "$size" && test -n "$pointer"
    pointer=${pointer#0x}
    grep -Eq "^\\+ ${size} [0-9a-f]+ ${pointer}$" "$darwin_raw_file"
    grep -Eq "^- ${pointer}$" "$darwin_raw_file"
done < "$darwin_pointer_file"
test "$(wc -l < "$darwin_pointer_file")" -eq 19
test "$(grep -c '^+ ' "$darwin_raw_file")" -eq 19
test "$(grep -c '^- ' "$darwin_raw_file")" -eq 19
