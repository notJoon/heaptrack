#!/bin/sh

# SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

launcher="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/heaptrack"
client="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/tst_macos_preload_client"
output_base="@CMAKE_CURRENT_BINARY_DIR@/tst macos launcher"
sip_output_base="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_sip"

expect_unsupported() {
    expected_option="$1"
    shift
    if output=$("$launcher" "$@" 2>&1); then
        echo "heaptrack unexpectedly accepted $expected_option on macOS"
        return 1
    fi
    echo "$output" | grep -F "heaptrack: $expected_option is not supported on macOS"
}

expect_unsupported --use-inject --use-inject /usr/bin/true
expect_unsupported --asan --asan /usr/bin/true
expect_unsupported --pid --pid $$

rm -f "$output_base.raw.gz" "$output_base.raw.zst"
"$launcher" --raw --quiet --output "$output_base" "$client"

if test -f "$output_base.raw.zst"; then
    zstd -dc "$output_base.raw.zst" > "$output_base.decoded"
else
    gzip -dc "$output_base.raw.gz" > "$output_base.decoded"
fi

grep -Eq '^\+ 12341 [0-9a-f]+ [0-9a-f]+$' "$output_base.decoded"
grep -Eq '^v [0-9a-f]+ 4$' "$output_base.decoded"
grep -Eq '^m [0-9a-f]+ .+ [0-9a-f]+ [0-9a-f]{32}( [0-9a-f]+ [0-9a-f]+)+$' "$output_base.decoded"

allocations=$(grep -c '^+ ' "$output_base.decoded")
frees=$(grep -c '^- ' "$output_base.decoded")
test "$allocations" -eq 7
test "$frees" -eq 7

rm -f "$sip_output_base.raw.gz" "$sip_output_base.raw.zst"
if sip_output=$("$launcher" --raw --quiet --output "$sip_output_base" /usr/bin/true 2>&1); then
    echo "heaptrack unexpectedly profiled a SIP-protected executable"
    exit 1
fi
echo "$sip_output" | grep -F "System Integrity Protection may block this executable"

started=$(date +%s)
if timeout_output=$("$launcher" --raw --quiet --output "$sip_output_base-timeout" /bin/sleep 10 2>&1); then
    echo "heaptrack unexpectedly waited for a SIP-protected executable"
    exit 1
fi
test "$(($(date +%s) - started))" -lt 9
echo "$timeout_output" | grep -F "System Integrity Protection may block this executable"

stress_output_base="@CMAKE_CURRENT_BINARY_DIR@/tst_macos_stress"
rm -f "$stress_output_base.raw.gz" "$stress_output_base.raw.zst"
"$launcher" --raw --quiet --output "$stress_output_base" "$client" --stress
test -s "$stress_output_base.raw.gz" || test -s "$stress_output_base.raw.zst"
