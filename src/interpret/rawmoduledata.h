/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef RAWMODULEDATA_H
#define RAWMODULEDATA_H

#include "util/linereader.h"

#include <cstdint>
#include <string>
#include <vector>

struct RawModuleSegment
{
    uint64_t virtualAddress = 0;
    uint64_t memorySize = 0;

    bool operator==(const RawModuleSegment& rhs) const
    {
        return virtualAddress == rhs.virtualAddress && memorySize == rhs.memorySize;
    }
};

struct RawModuleData
{
    std::string fileName;
    uint64_t addressStart = 0;
    std::string uuid;
    std::vector<RawModuleSegment> segments;
};

inline bool isValidModuleUuid(const std::string& uuid)
{
    if (uuid == "-") {
        return true;
    }
    if (uuid.size() != 32) {
        return false;
    }
    for (const auto character : uuid) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool parseRawModule(LineReader& reader, unsigned int fileVersion, RawModuleData& module)
{
    module = {};
    if (!(reader >> module.fileName)) {
        return false;
    }
    if (module.fileName == "-") {
        return true;
    }
    if (!(reader >> module.addressStart)) {
        return false;
    }
    if (fileVersion >= 4 && (!reader.readToken(module.uuid) || !isValidModuleUuid(module.uuid))) {
        return false;
    }

    while (true) {
        RawModuleSegment segment;
        if (!(reader >> segment.virtualAddress)) {
            return true;
        }
        if (!(reader >> segment.memorySize)) {
            return false;
        }
        module.segments.push_back(segment);
    }
}

#endif // RAWMODULEDATA_H
