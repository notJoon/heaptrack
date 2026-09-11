/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

/*
 * @file heaptrack_interpose.cpp
 * Replaces macOS allocation functions and records their results with heaptrack.
 */

#include "libheaptrack.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <malloc/malloc.h>
#include <unistd.h>

namespace {

/*
 * Indicates whether allocation events can be recorded safely.
 */
std::atomic<bool> isInitialized {false};

/*
 * Registers a replacement function in the Mach O interpose section.
 */
#define HEAPTRACK_INTERPOSE(replacement, replacee)                                                                     \
    __attribute__((used)) static const struct                                                                          \
    {                                                                                                                  \
        const void* replacement;                                                                                       \
        const void* replacee;                                                                                          \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {                                          \
        reinterpret_cast<const void*>(&replacement), reinterpret_cast<const void*>(&replacee)}

void initialize() __attribute__((constructor));

/*
 * Stops tracking once when the process exits.
 */
void shutdownTracking()
{
    if (isInitialized.exchange(false, std::memory_order_acq_rel)) {
        heaptrack_stop();
    }
}

/*
 * Updates local state when the heaptrack core stops tracking.
 */
void trackingStopped()
{
    isInitialized.store(false, std::memory_order_release);
}

/*
 * Notifies the launcher that the preload library is ready.
 */
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

/*
 * Initializes tracking when the preload library is loaded.
 */
void initialize()
{
    if (!heaptrack_init(
        getenv("DUMP_HEAPTRACK_OUTPUT"),
        [] {
            unsetenv("DYLD_INSERT_LIBRARIES");
            unsetenv("DUMP_HEAPTRACK_OUTPUT");
        },
        nullptr, &trackingStopped)) {
        return;
    }
    atexit(&shutdownTracking);
    isInitialized.store(true, std::memory_order_release);
    reportReady();
}

/*
 * Returns whether allocator calls should be recorded.
 */
bool shouldTrack()
{
    return isInitialized.load(std::memory_order_acquire);
}

/*
 * Allocates memory and records the returned pointer.
 */
void* heaptrackMalloc(size_t size)
{
    void* pointer = malloc(size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

/*
 * Records a release before freeing the pointer.
 */
void heaptrackFree(void* pointer)
{
    const int savedErrno = errno;
    if (shouldTrack()) {
        heaptrack_free(pointer);
    }
    free(pointer);
    errno = savedErrno;
}

/*
 * Allocates zeroed memory and records its total size.
 */
void* heaptrackCalloc(size_t count, size_t size)
{
    void* pointer = calloc(count, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, count * size);
    }
    return pointer;
}

/*
 * Reallocates memory while keeping pointer reuse and recording synchronized.
 */
void* heaptrackRealloc(void* oldPointer, size_t size)
{
    if (!shouldTrack()) {
        return realloc(oldPointer, size);
    }
    return heaptrack_realloc_locked(
        oldPointer, size, [](void* pointer, size_t allocationSize, void*) { return realloc(pointer, allocationSize); },
        nullptr);
}

/*
 * Allocates aligned memory and records successful requests.
 */
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

/*
 * Allocates page aligned memory and records the returned pointer.
 */
void* heaptrackValloc(size_t size)
{
    void* pointer = valloc(size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

/*
 * Allocates aligned memory and records the returned pointer.
 */
void* heaptrackAlignedAlloc(size_t alignment, size_t size)
{
    void* pointer = aligned_alloc(alignment, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

/*
 * Allocates memory from a zone and records the returned pointer.
 */
void* heaptrackZoneMalloc(malloc_zone_t* zone, size_t size)
{
    void* pointer = malloc_zone_malloc(zone, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

/*
 * Allocates zeroed memory from a zone and records its total size.
 */
void* heaptrackZoneCalloc(malloc_zone_t* zone, size_t count, size_t size)
{
    void* pointer = malloc_zone_calloc(zone, count, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, count * size);
    }
    return pointer;
}

/*
 * Records a release before freeing a pointer from its zone.
 */
void heaptrackZoneFree(malloc_zone_t* zone, void* pointer)
{
    const int savedErrno = errno;
    if (shouldTrack()) {
        heaptrack_free(pointer);
    }
    malloc_zone_free(zone, pointer);
    errno = savedErrno;
}

/*
 * Reallocates memory in a zone while keeping recording synchronized.
 */
void* heaptrackZoneRealloc(malloc_zone_t* zone, void* oldPointer, size_t size)
{
    if (!shouldTrack()) {
        return malloc_zone_realloc(zone, oldPointer, size);
    }
    return heaptrack_realloc_locked(
        oldPointer, size,
        [](void* pointer, size_t allocationSize, void* context) {
            return malloc_zone_realloc(static_cast<malloc_zone_t*>(context), pointer, allocationSize);
        },
        zone);
}

/*
 * Allocates page aligned memory from a zone and records it.
 */
void* heaptrackZoneValloc(malloc_zone_t* zone, size_t size)
{
    void* pointer = malloc_zone_valloc(zone, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

/*
 * Allocates aligned memory from a zone and records it.
 */
void* heaptrackZoneMemalign(malloc_zone_t* zone, size_t alignment, size_t size)
{
    void* pointer = malloc_zone_memalign(zone, alignment, size);
    if (shouldTrack()) {
        heaptrack_malloc(pointer, size);
    }
    return pointer;
}

#if HEAPTRACK_HAVE_MALLOC_TYPE
/*
 * Records a release made through the typed allocation API.
 */
void heaptrackTypeFree(void* pointer, malloc_type_id_t typeId)
{
    const int savedErrno = errno;
    if (shouldTrack()) {
        heaptrack_free(pointer);
    }
    malloc_type_free(pointer, typeId);
    errno = savedErrno;
}

/*
 * Records a typed release made through a specific zone.
 */
void heaptrackTypeZoneFree(malloc_zone_t* zone, void* pointer, malloc_type_id_t typeId)
{
    const int savedErrno = errno;
    if (shouldTrack()) {
        heaptrack_free(pointer);
    }
    malloc_type_zone_free(zone, pointer, typeId);
    errno = savedErrno;
}
#endif

/*
 * Connects each macOS allocator symbol to its heaptrack replacement.
 */
HEAPTRACK_INTERPOSE(heaptrackMalloc, malloc);
HEAPTRACK_INTERPOSE(heaptrackFree, free);
HEAPTRACK_INTERPOSE(heaptrackCalloc, calloc);
HEAPTRACK_INTERPOSE(heaptrackRealloc, realloc);
HEAPTRACK_INTERPOSE(heaptrackPosixMemalign, posix_memalign);
HEAPTRACK_INTERPOSE(heaptrackValloc, valloc);
HEAPTRACK_INTERPOSE(heaptrackAlignedAlloc, aligned_alloc);
HEAPTRACK_INTERPOSE(heaptrackZoneMalloc, malloc_zone_malloc);
HEAPTRACK_INTERPOSE(heaptrackZoneCalloc, malloc_zone_calloc);
HEAPTRACK_INTERPOSE(heaptrackZoneFree, malloc_zone_free);
HEAPTRACK_INTERPOSE(heaptrackZoneRealloc, malloc_zone_realloc);
HEAPTRACK_INTERPOSE(heaptrackZoneValloc, malloc_zone_valloc);
HEAPTRACK_INTERPOSE(heaptrackZoneMemalign, malloc_zone_memalign);
#if HEAPTRACK_HAVE_MALLOC_TYPE
HEAPTRACK_INTERPOSE(heaptrackTypeFree, malloc_type_free);
HEAPTRACK_INTERPOSE(heaptrackTypeZoneFree, malloc_type_zone_free);
#endif

}
