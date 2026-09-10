/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

extern "C" __attribute__((noinline)) int heaptrack_symbolication_target(int value)
{
    return value + 1;
}

extern "C" __attribute__((noinline)) int heaptrack_symbolication_target_two(int value)
{
    return value + 2;
}

int main()
{
    return heaptrack_symbolication_target(0) + heaptrack_symbolication_target_two(0) - 3;
}
