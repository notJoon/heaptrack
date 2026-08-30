/*
    SPDX-FileCopyrightText: 2018 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "3rdparty/doctest.h"

#include "track/libheaptrack.h"
#ifdef __APPLE__
#include "track/machmodulecache.h"
#endif
#include "util/linewriter.h"

#include <cmath>
#include <cstdio>
#include <dlfcn.h>

#include <atomic>
#include <future>
#include <iostream>
#include <regex>
#include <thread>
#include <vector>

#include "tempfile.h"

bool initBeforeCalled = false;
bool initAfterCalled = false;
bool stopCalled = false;

namespace {
std::atomic<bool> reallocCallbackEntered {false};
std::atomic<bool> releaseReallocCallback {false};
void* reallocCallbackResult = nullptr;

void* blockingRealloc(void*, size_t)
{
    reallocCallbackEntered.store(true, std::memory_order_release);
    while (!releaseReallocCallback.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return reallocCallbackResult;
}
}

using namespace std;

TEST_CASE ("api") {
    TempFile tmp; // opened/closed by heaptrack_init

    SUBCASE("init")
    {
        heaptrack_init(
            tmp.fileName.c_str(),
            []() {
                REQUIRE(!initBeforeCalled);
                REQUIRE(!initAfterCalled);
                REQUIRE(!stopCalled);
                initBeforeCalled = true;
            },
            [](LineWriter& /*out*/) {
                REQUIRE(initBeforeCalled);
                REQUIRE(!initAfterCalled);
                REQUIRE(!stopCalled);
                initAfterCalled = true;
            },
            []() {
                REQUIRE(initBeforeCalled);
                REQUIRE(initAfterCalled);
                REQUIRE(!stopCalled);
                stopCalled = true;
            });

        REQUIRE(initBeforeCalled);
        REQUIRE(initAfterCalled);
        REQUIRE(!stopCalled);

        int data[2] = {0};

        SUBCASE("no-op-malloc")
        {
            heaptrack_malloc(0, 0);
        }
        SUBCASE("no-op-malloc-free")
        {
            heaptrack_free(0);
        }
        SUBCASE("no-op-malloc-realloc")
        {
            heaptrack_realloc(data, 1, 0);
        }

        SUBCASE("malloc-free")
        {
            heaptrack_malloc(data, 4);
            heaptrack_free(data);
        }

        SUBCASE("realloc")
        {
            heaptrack_malloc(data, 4);
            heaptrack_realloc(data, 8, data);
            heaptrack_realloc(data, 16, data + 1);
            heaptrack_free(data + 1);
        }

        SUBCASE("realloc serializes address reuse")
        {
            reallocCallbackEntered.store(false, memory_order_relaxed);
            releaseReallocCallback.store(false, memory_order_relaxed);
            reallocCallbackResult = data + 1;
            heaptrack_malloc(data, sizeof(data[0]));

            auto reallocator = async(launch::async, [&]() {
                return heaptrack_realloc_locked(data, sizeof(data[1]), &blockingRealloc);
            });
            while (!reallocCallbackEntered.load(memory_order_acquire)) {
                this_thread::yield();
            }

            atomic<bool> allocatorStarted {false};
            atomic<bool> reusedAddressRecorded {false};
            auto allocator = async(launch::async, [&]() {
                allocatorStarted.store(true, memory_order_release);
                heaptrack_malloc(data, sizeof(data[0]));
                reusedAddressRecorded.store(true, memory_order_release);
            });
            while (!allocatorStarted.load(memory_order_acquire)) {
                this_thread::yield();
            }
            this_thread::sleep_for(chrono::milliseconds(20));
            const bool wasSerialized = !reusedAddressRecorded.load(memory_order_acquire);

            releaseReallocCallback.store(true, memory_order_release);
            REQUIRE(reallocator.get() == data + 1);
            allocator.get();
            REQUIRE(wasSerialized);
            REQUIRE(reusedAddressRecorded.load(memory_order_acquire));
            heaptrack_free(data);
            heaptrack_free(data + 1);
        }

        SUBCASE("invalidate-cache")
        {
            heaptrack_invalidate_module_cache(nullptr);

            static bool wasCalled = false;
            heaptrack_invalidate_module_cache([]() { wasCalled = true; });
            REQUIRE(wasCalled);
        }

        SUBCASE("multi-threaded")
        {
            const auto numThreads = min(4u, thread::hardware_concurrency());

            cout << "start threads" << endl;
            {
                vector<future<void>> futures;
                for (unsigned i = 0; i < numThreads; ++i) {
                    futures.emplace_back(async(launch::async, []() {
                        for (int i = 0; i < 10000; ++i) {
                            heaptrack_malloc(&i, i);
                            heaptrack_realloc(&i, i + 1, &i);
                            heaptrack_free(&i);
                            if (i % 100 == 0) {
                                heaptrack_invalidate_module_cache(nullptr);
                            }
                        }
                    }));
                }
            }
            cout << "threads finished" << endl;
        }

        SUBCASE("stop")
        {
            heaptrack_stop();
            REQUIRE(stopCalled);
        }
    }
}

#ifdef __APPLE__
TEST_CASE ("Mach-O module cache snapshots") {
    MachModuleCache cache;
    const auto* header = _dyld_get_image_header(0);
    REQUIRE(header);
    REQUIRE(cache.addImage(header, _dyld_get_image_vmaddr_slide(0)));

    struct Snapshot
    {
        size_t count = 0;
        bool hasUuid = false;
        bool hasText = false;
    } snapshot;
    REQUIRE(cache.forEach(
        [](const MachModuleCache::Module& module, void* context) {
            auto& snapshot = *static_cast<Snapshot*>(context);
            ++snapshot.count;
            snapshot.hasUuid = module.uuid[0] != '\0';
            snapshot.hasText = module.textSize != 0;
            return true;
        },
        &snapshot));
    REQUIRE(snapshot.count == 1);
    REQUIRE(snapshot.hasUuid);
    REQUIRE(snapshot.hasText);

    cache.removeImage(header);
    snapshot = {};
    REQUIRE(cache.forEach(
        [](const MachModuleCache::Module&, void* context) {
            ++static_cast<Snapshot*>(context)->count;
            return true;
        },
        &snapshot));
    REQUIRE(snapshot.count == 0);
}

TEST_CASE ("macOS process metadata") {
    TempFile tmp;
    heaptrack_init(tmp.fileName.c_str(), nullptr, nullptr, nullptr);

    int data = 0;
    heaptrack_malloc(&data, sizeof(data));
    heaptrack_free(&data);

    const auto module = dlopen(HEAPTRACK_TEST_DYLIB, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(module);
    heaptrack_malloc(&data, sizeof(data));
    heaptrack_free(&data);
    REQUIRE(dlclose(module) == 0);
    heaptrack_malloc(&data, sizeof(data));
    heaptrack_free(&data);

    heaptrack_stop();

    const auto contents = tmp.readContents();
    REQUIRE(regex_search(contents, regex("^v [0-9a-f]+ 4\\n")));
    REQUIRE(contents.find("\nx ") != string::npos);
    REQUIRE(contents.find("\nX ") != string::npos);
    REQUIRE(contents.find("\nI ") != string::npos);
    REQUIRE(contents.find("\nR ") != string::npos);

    size_t moduleCacheResets = 0;
    vector<string> moduleSnapshots;
    for (auto offset = contents.find("\nm 1 -\n"); offset != string::npos;
         offset = contents.find("\nm 1 -\n", offset + 1)) {
        ++moduleCacheResets;
        const auto next = contents.find("\nm 1 -\n", offset + 1);
        moduleSnapshots.push_back(contents.substr(offset, next - offset));
    }
    REQUIRE(moduleCacheResets >= 3);
    REQUIRE(moduleSnapshots.front().find("libtestlib_indirect.dylib") == string::npos);
    REQUIRE(moduleSnapshots[moduleSnapshots.size() - 2].find("libtestlib_indirect.dylib") != string::npos);
    REQUIRE(moduleSnapshots.back().find("libtestlib_indirect.dylib") == string::npos);

    smatch moduleMatch;
    const regex modulePattern(
        "\\nm [0-9a-f]+ [^\\n]*libtestlib_indirect\\.dylib [0-9a-f]+ ([0-9a-f]{32}) [0-9a-f]+ [0-9a-f]+\\n");
    REQUIRE(regex_search(contents, moduleMatch, modulePattern));
    REQUIRE(moduleMatch[1].str() != "00000000000000000000000000000000");
}

TEST_CASE ("concurrent macOS module updates") {
    TempFile tmp;
    heaptrack_init(tmp.fileName.c_str(), nullptr, nullptr, nullptr);

    atomic<bool> loaderStarted {false};
    atomic<bool> loaderFinished {false};
    atomic<bool> loaderSucceeded {true};

    thread loader([&]() {
        loaderStarted.store(true, memory_order_release);
        for (int i = 0; i < 2000; ++i) {
            const auto module = dlopen(HEAPTRACK_TEST_DYLIB, RTLD_NOW | RTLD_LOCAL);
            if (!module || dlclose(module) != 0) {
                loaderSucceeded.store(false, memory_order_relaxed);
                break;
            }
        }
        loaderFinished.store(true, memory_order_release);
    });

    while (!loaderStarted.load(memory_order_acquire)) { }
    int data = 0;
    while (!loaderFinished.load(memory_order_acquire)) {
        heaptrack_malloc(&data, sizeof(data));
        heaptrack_free(&data);
    }

    loader.join();
    heaptrack_stop();
    REQUIRE(loaderSucceeded.load(memory_order_relaxed));
}
#endif
