/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "libheaptrack.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace {

std::atomic<bool> isInitialized {false};

#define HEAPTRACK_INTERPOSE(replacement, replacee)                                                                    \
    __attribute__((used)) static const struct                                                                         \
    {                                                                                                                  \
        const void* replacement;                                                                                       \
        const void* replacee;                                                                                          \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {                                         \
        reinterpret_cast<const void*>(&replacement), reinterpret_cast<const void*>(&replacee)}

void initialize() __attribute__((constructor));

void shutdownTracking()
{
    if (isInitialized.exchange(false, std::memory_order_acq_rel)) {
        heaptrack_stop();
    }
}

void trackingStopped()
{
    isInitialized.store(false, std::memory_order_release);
}

void reportReady()
{
    const char* readyFile = getenv("DUMP_HEAPTRACK_READY");
    if (!readyFile || !readyFile[0]) {
        return;
    }

    const int descriptor = open(readyFile, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor != -1) {
        static constexpr char ready = '1';
        (void)write(descriptor, &ready, sizeof(ready));
        close(descriptor);
    }
    unsetenv("DUMP_HEAPTRACK_READY");
}

void initialize()
{
    heaptrack_init(
        getenv("DUMP_HEAPTRACK_OUTPUT"),
        [] {
            unsetenv("DYLD_INSERT_LIBRARIES");
            unsetenv("DUMP_HEAPTRACK_OUTPUT");
        },
        nullptr, &trackingStopped);
    atexit(&shutdownTracking);
    isInitialized.store(true, std::memory_order_release);
    reportReady();
}

bool shouldTrack()
{
    return isInitialized.load(std::memory_order_acquire);
}

void* heaptrackMalloc(size_t size)
{
    void* pointer = malloc(size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

void heaptrackFree(void* pointer)
{
    const int savedErrno = errno;
    if (shouldTrack()) {
        heaptrack_free(pointer);
    }
    free(pointer);
    errno = savedErrno;
}

void* heaptrackCalloc(size_t count, size_t size)
{
    void* pointer = calloc(count, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, count * size);
    }
    return pointer;
}

void* heaptrackRealloc(void* oldPointer, size_t size)
{
    if (!shouldTrack()) {
        return realloc(oldPointer, size);
    }
    return heaptrack_realloc_locked(oldPointer, size, &realloc);
}

int heaptrackPosixMemalign(void** pointer, size_t alignment, size_t size)
{
    const int savedErrno = errno;
    const int result = posix_memalign(pointer, alignment, size);
    if (shouldTrack() && result == 0) {
        heaptrack_malloc(*pointer, size);
    }
    errno = savedErrno;
    return result;
}

void* heaptrackValloc(size_t size)
{
    void* pointer = valloc(size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

void* heaptrackAlignedAlloc(size_t alignment, size_t size)
{
    void* pointer = aligned_alloc(alignment, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

HEAPTRACK_INTERPOSE(heaptrackMalloc, malloc);
HEAPTRACK_INTERPOSE(heaptrackFree, free);
HEAPTRACK_INTERPOSE(heaptrackCalloc, calloc);
HEAPTRACK_INTERPOSE(heaptrackRealloc, realloc);
HEAPTRACK_INTERPOSE(heaptrackPosixMemalign, posix_memalign);
HEAPTRACK_INTERPOSE(heaptrackValloc, valloc);
HEAPTRACK_INTERPOSE(heaptrackAlignedAlloc, aligned_alloc);

}
