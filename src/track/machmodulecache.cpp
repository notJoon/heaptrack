/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "machmodulecache.h"

#include <dlfcn.h>
#include <mach-o/loader.h>

#include <cstring>

namespace {

bool readModule(const mach_header* header, intptr_t slide, MachModuleCache::Module& module)
{
    if (!header || header->magic != MH_MAGIC_64) {
        return false;
    }

    const auto* header64 = reinterpret_cast<const mach_header_64*>(header);
    const auto* command = reinterpret_cast<const load_command*>(header64 + 1);
    const auto* commandsEnd = reinterpret_cast<const char*>(command) + header64->sizeofcmds;
    if (header64->ncmds && header64->sizeofcmds < sizeof(load_command)) {
        return false;
    }

    const segment_command_64* textSegment = nullptr;
    const uuid_command* uuidCommand = nullptr;
    for (uint32_t commandIndex = 0; commandIndex < header64->ncmds; ++commandIndex) {
        const auto* commandAddress = reinterpret_cast<const char*>(command);
        if (commandAddress > commandsEnd - sizeof(load_command) || command->cmdsize < sizeof(load_command)
            || command->cmdsize > static_cast<size_t>(commandsEnd - commandAddress)) {
            return false;
        }
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(segment_command_64)) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if (strncmp(segment->segname, SEG_TEXT, sizeof(segment->segname)) == 0) {
                textSegment = segment;
            }
        } else if (command->cmd == LC_UUID && command->cmdsize >= sizeof(uuid_command)) {
            uuidCommand = reinterpret_cast<const uuid_command*>(command);
        }
        command = reinterpret_cast<const load_command*>(commandAddress + command->cmdsize);
    }

    Dl_info imageInfo = {};
    const auto* fileName = dladdr(header, &imageInfo) && imageInfo.dli_fname ? imageInfo.dli_fname : "x";
    strlcpy(module.fileName, fileName, sizeof(module.fileName));

    constexpr char hexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        const auto byte = uuidCommand ? uuidCommand->uuid[i] : 0;
        module.uuid[2 * i] = hexDigits[byte >> 4];
        module.uuid[2 * i + 1] = hexDigits[byte & 0xf];
    }
    module.uuid[32] = '\0';
    module.slide = static_cast<uintptr_t>(slide);
    if (textSegment) {
        module.textAddress = textSegment->vmaddr;
        module.textSize = textSegment->vmsize;
    }
    return true;
}

}

bool MachModuleCache::addImage(const mach_header* header, intptr_t slide)
{
    Module module;
    if (!readModule(header, slide, module) || pthread_mutex_lock(&m_mutex) != 0) {
        return false;
    }

    Entry* available = nullptr;
    for (auto& entry : m_entries) {
        if (entry.active && entry.header == header) {
            available = &entry;
            break;
        }
        if (!entry.active && !available) {
            available = &entry;
        }
    }
    if (available) {
        available->header = header;
        available->module = module;
        available->active = true;
    }
    pthread_mutex_unlock(&m_mutex);
    return available != nullptr;
}

void MachModuleCache::removeImage(const mach_header* header)
{
    if (pthread_mutex_lock(&m_mutex) != 0) {
        return;
    }
    for (auto& entry : m_entries) {
        if (entry.active && entry.header == header) {
            entry.active = false;
            break;
        }
    }
    pthread_mutex_unlock(&m_mutex);
}

bool MachModuleCache::forEach(Callback callback, void* context) const
{
    if (!callback || pthread_mutex_lock(&m_mutex) != 0) {
        return false;
    }
    bool success = true;
    for (const auto& entry : m_entries) {
        if (entry.active && !callback(entry.module, context)) {
            success = false;
            break;
        }
    }
    pthread_mutex_unlock(&m_mutex);
    return success;
}
