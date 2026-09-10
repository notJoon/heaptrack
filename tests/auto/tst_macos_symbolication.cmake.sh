#!/bin/sh

# SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

INTERPRET="@PROJECT_BINARY_DIR@/@LIBEXEC_INSTALL_DIR@/heaptrack_interpret"
FIXTURE="@PROJECT_BINARY_DIR@/@BIN_INSTALL_DIR@/tst_macos_symbolication_fixture"
SLIDE=1000

uuid=$(/usr/bin/dwarfdump --uuid "$FIXTURE" | awk '{print tolower($2)}' | tr -d -)
symbol=$(/usr/bin/nm -n "$FIXTURE" | awk '$3 == "_heaptrack_symbolication_target" {print $1}')
symbol_two=$(/usr/bin/nm -n "$FIXTURE" | awk '$3 == "_heaptrack_symbolication_target_two" {print $1}')
text_vmaddr=$(/usr/bin/otool -l "$FIXTURE" | awk '$1 == "segname" && $2 == "__TEXT" {text=1} text && $1 == "vmaddr" {print $2; exit}')
text_vmsize=$(/usr/bin/otool -l "$FIXTURE" | awk '$1 == "segname" && $2 == "__TEXT" {text=1} text && $1 == "vmsize" {print $2; exit}')
ip=$(printf '%x' "$((0x$symbol + 0x$SLIDE))")
ip_two=$(printf '%x' "$((0x$symbol_two + 0x$SLIDE))")
name_size=$(printf '%x' "${#FIXTURE}")
actual=$(mktemp)
diagnostic=$(mktemp)
trap 'rm -f -- "$actual" "$diagnostic"' EXIT

{
    echo "v 10550 4"
    echo "m $name_size $FIXTURE $SLIDE $uuid ${text_vmaddr#0x} ${text_vmsize#0x}"
    echo "t $ip 0"
    echo "t $ip_two 1"
} | "$INTERPRET" > "$actual"

grep -Eq '^s [0-9a-f]+ heaptrack_symbolication_target$' "$actual"
grep -Eq '^s [0-9a-f]+ heaptrack_symbolication_target_two$' "$actual"
grep -Eq '^s [0-9a-f]+ .*/tst_macos_symbolication_fixture.cpp$' "$actual"
grep -Eq "^i $ip 1 [0-9a-f]+ [0-9a-f]+ [0-9a-f]+$" "$actual"

bad_uuid=00000000000000000000000000000000
{
    echo "v 10550 4"
    echo "m $name_size $FIXTURE $SLIDE $bad_uuid ${text_vmaddr#0x} ${text_vmsize#0x}"
    echo "t $ip 0"
} | "$INTERPRET" > /dev/null 2> "$diagnostic"

grep -F "no Mach-O binary or dSYM matching UUID $bad_uuid for $FIXTURE" "$diagnostic"
