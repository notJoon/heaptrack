/*
    SPDX-FileCopyrightText: 2026 notJoon

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef MACHMODULECACHE_H
#define MACHMODULECACHE_H

#include <mach-o/dyld.h>
#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <limits.h>

class MachModuleCache
{
public:
    struct Module
    {
        char fileName[PATH_MAX] = {};
        char uuid[33] = {};
        uintptr_t slide = 0;
        uintptr_t textAddress = 0;
        uintptr_t textSize = 0;
    };

    using Callback = bool (*)(const Module&, void*);

    bool addImage(const mach_header* header, intptr_t slide);
    void removeImage(const mach_header* header);
    bool forEach(Callback callback, void* context) const;

private:
    enum : size_t
    {
        Capacity = 1024
    };

    struct Entry
    {
        const mach_header* header = nullptr;
        Module module;
        bool active = false;
    };

    mutable pthread_mutex_t m_mutex = PTHREAD_MUTEX_INITIALIZER;
    Entry m_entries[Capacity] = {};
};

#endif // MACHMODULECACHE_H
