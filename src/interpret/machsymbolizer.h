/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef MACHSYMBOLIZER_H
#define MACHSYMBOLIZER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/types.h>

class MachSymbolizer
{
public:
    struct Frame
    {
        std::string function;
        std::string file;
        int line = 0;
    };

    MachSymbolizer(const std::string& fileName, const std::string& uuid, uintptr_t slide,
                   const std::vector<std::string>& searchPaths);
    ~MachSymbolizer();

    MachSymbolizer(const MachSymbolizer&) = delete;
    MachSymbolizer& operator=(const MachSymbolizer&) = delete;

    Frame resolve(uintptr_t address);

private:
    FILE* m_input = nullptr;
    FILE* m_output = nullptr;
    pid_t m_pid = -1;
};

#endif
