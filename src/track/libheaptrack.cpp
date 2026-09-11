/*
    SPDX-FileCopyrightText: 2014-2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

/**
 * @file libheaptrack.cpp
 *
 * @brief Collect raw heaptrack data by overloading heap allocation functions.
 */

#include "libheaptrack.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#ifndef __APPLE__
#include <link.h>
#endif
#include <pthread.h>
#include <signal.h>
#ifdef __linux__
#include <stdio_ext.h>
#include <syscall.h>
#endif
#ifdef __FreeBSD__
#include <libutil.h>
#include <pthread_np.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#endif
#ifdef __APPLE__
#include "machmodulecache.h"
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "tracetree.h"
#include "util/config.h"
#include "util/libunwind_config.h"
#include "util/linewriter.h"
#include "util/macroutils.h"

extern "C" {
// see upstream "documentation" at:
// https://github.com/llvm-mirror/compiler-rt/blob/master/include/sanitizer/lsan_interface.h#L76
#ifdef __APPLE__
__attribute__((weak_import)) const char* __lsan_default_suppressions();
#else
__attribute__((weak)) const char* __lsan_default_suppressions();
#endif
}
#ifdef __GLIBCXX__
namespace __gnu_cxx {
__attribute__((weak)) extern void __freeres();
}
#endif

/**
 * uncomment this to get extended debug code for known pointers
 * there are still some malloc functions I'm missing apparently,
 * related to TLS and such I guess
 */
// #define DEBUG_MALLOC_PTRS

#ifdef DEBUG_MALLOC_PTRS
#include <tsl/robin_set.h>
#endif

using namespace std;

namespace {

using clock = chrono::steady_clock;
chrono::time_point<clock> startTime()
{
    static const chrono::time_point<clock> s_start = clock::now();
    return s_start;
}

chrono::milliseconds elapsedTime()
{
    return chrono::duration_cast<chrono::milliseconds>(clock::now() - startTime());
}

uint64_t gettid()
{
#ifdef __linux__
    return static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(__FreeBSD__)
    return static_cast<uint64_t>(pthread_getthreadid_np());
#elif defined(__APPLE__)
    uint64_t threadId = 0;
    pthread_threadid_np(nullptr, &threadId);
    return threadId;
#endif
}

/**
 * A per-thread handle guard to prevent infinite recursion, which should be
 * acquired before doing any special symbol handling.
 */
struct RecursionGuard
{
    RecursionGuard()
        : wasLocked(activate())
    {
    }

    ~RecursionGuard()
    {
        setActive(wasLocked);
    }

    const bool wasLocked;
    static bool isActive()
    {
#ifdef __APPLE__
        return s_keyFailed.load(memory_order_acquire) || !s_keyReady.load(memory_order_acquire)
            || pthread_getspecific(s_key) != nullptr;
#else
        return s_isActive;
#endif
    }

    static void setActive(bool active)
    {
#ifdef __APPLE__
        if (s_keyReady.load(memory_order_acquire)) {
            if (pthread_setspecific(s_key, active ? reinterpret_cast<void*>(1) : nullptr) != 0) {
                s_keyFailed.store(true, memory_order_release);
            }
        }
#else
        s_isActive = active;
#endif
    }

private:
    static bool activate()
    {
#ifdef __APPLE__
        pthread_once(&s_keyOnce, &initializeKey);
#endif
        const auto wasActive = isActive();
        setActive(true);
        return wasActive;
    }

#ifdef __APPLE__
    static void initializeKey()
    {
        if (pthread_key_create(&s_key, nullptr) == 0) {
            s_keyReady.store(true, memory_order_release);
        }
    }

    static pthread_key_t s_key;
    static pthread_once_t s_keyOnce;
    static atomic<bool> s_keyReady;
    static atomic<bool> s_keyFailed;
#else
    static thread_local bool s_isActive;
#endif
};

#ifdef __APPLE__
pthread_key_t RecursionGuard::s_key;
pthread_once_t RecursionGuard::s_keyOnce = PTHREAD_ONCE_INIT;
atomic<bool> RecursionGuard::s_keyReady {false};
atomic<bool> RecursionGuard::s_keyFailed {false};
#else
thread_local bool RecursionGuard::s_isActive = false;
#endif

enum DebugVerbosity
{
    WarningOutput,
    NoDebugOutput,
    MinimalOutput,
    VerboseOutput,
    VeryVerboseOutput,
};

// change this to add more debug output to stderr
constexpr const DebugVerbosity s_debugVerbosity = NoDebugOutput;

/**
 * Call this to optionally show debug information but give the compiler
 * a hand in removing it all if debug output is disabled.
 */
template <DebugVerbosity debugLevel, typename Callback>
inline void debugLog(Callback callback)
{
    if (debugLevel <= s_debugVerbosity) {
        RecursionGuard guard;
        flockfile(stderr);
        if (debugLevel == WarningOutput) {
            fprintf(stderr, "heaptrack warning [%d:%" PRIu64 "]@%" PRIu64 " ", getpid(), gettid(),
                    elapsedTime().count());
        } else {
            fprintf(stderr, "heaptrack debug(%d) [%d:%" PRIu64 "]@%" PRIu64 " ", debugLevel, getpid(), gettid(),
                    elapsedTime().count());
        }
        callback(stderr);
        fputc('\n', stderr);
        funlockfile(stderr);
    }
}

/**
 * Call this to optionally show debug information but give the compiler
 * a hand in removing it all if debug output is disabled.
 */
template <DebugVerbosity debugLevel, typename... Args>
inline void debugLog(const char fmt[], Args... args)
{
    debugLog<debugLevel>([&](FILE* out) { fprintf(out, fmt, args...); });
}

void printBacktrace()
{
    if (s_debugVerbosity == NoDebugOutput)
        return;

    RecursionGuard guard;

    Trace::print();
}

/**
 * Set to true in an atexit handler. In such conditions, the stop callback
 * will not be called.
 */
atomic<bool> s_atexit {false};

/**
 * Set to true in heaptrack_stop, when s_atexit was not yet set. In such conditions,
 * we always fully unload and cleanup behind ourselves
 */
atomic<bool> s_forceCleanup {false};

// based on: https://stackoverflow.com/a/24315631/35250
void replaceAll(string& str, const string& search, const string& replace)
{
    size_t start_pos = 0;
    while ((start_pos = str.find(search, start_pos)) != string::npos) {
        str.replace(start_pos, search.length(), replace);
        start_pos += replace.length();
    }
}

// see https://bugs.kde.org/show_bug.cgi?id=408547
// apparently sometimes flock can return EAGAIN, despite that not being a documented return value
static int lockFile(int fd)
{
    int ret = -1;
    while ((ret = flock(fd, LOCK_EX | LOCK_NB)) == EAGAIN) {
        // try again
    }
    return ret;
}

int createFile(const char* fileName)
{
    string outputFileName;
    if (fileName) {
        outputFileName.assign(fileName);
    }

    if (outputFileName == "-" || outputFileName == "stdout") {
        debugLog<VerboseOutput>("%s", "will write to stdout");
        return fileno(stdout);
    } else if (outputFileName == "stderr") {
        debugLog<VerboseOutput>("%s", "will write to stderr");
        return fileno(stderr);
    }

    if (outputFileName.empty()) {
        // env var might not be set when linked directly into an executable
        outputFileName = "heaptrack.$$";
    }

    replaceAll(outputFileName, "$$", to_string(getpid()));

    auto out = open(outputFileName.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    debugLog<VerboseOutput>("will write to %s/%p\n", outputFileName.c_str(), out);
    // we do our own locking, this speeds up the writing significantly
    if (out == -1) {
        fprintf(stderr, "ERROR: failed to open heaptrack output file %s: %s (%d)\n", outputFileName.c_str(),
                strerror(errno), errno);
    } else if (lockFile(out) != 0) {
#if defined(__FreeBSD__) || defined(__APPLE__)
        // pipes do not support flock, create a regular file
        auto lockpath = outputFileName + ".lock";
        auto lockfile = open(lockpath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        debugLog<VerboseOutput>("will lock %s/%p\n", lockpath.c_str(), lockfile);
        if (lockFile(lockfile) == 0) {
            // leaking the fd seems fine
            return out;
        }
#endif
        fprintf(stderr, "ERROR: failed to lock heaptrack output file %s: %s (%d)\n", outputFileName.c_str(),
                strerror(errno), errno);
        close(out);
        return -1;
    }

    return out;
}

/**
 * Thread-Safe heaptrack API
 *
 * The only critical section in libheaptrack is the output of the data,
 * module enumeration, as well as initialization and shutdown.
 */
class HeapTrack
{
public:
    template <typename Op>
    static bool op(const RecursionGuard& /*recursionGuard*/, const Op& op)
    {
        debugLog<VeryVerboseOutput>("%s", "acquiring lock");

        auto locked = tryLock([]() { return s_forceCleanup.load(); });
        if (!locked) {
            return false;
        }

        debugLog<VeryVerboseOutput>("%s", "lock acquired");

        HeapTrack heaptrack(locked);
        op(heaptrack);

        return true;
    }

    ~HeapTrack()
    {
        debugLog<VeryVerboseOutput>("%s", "releasing lock");

        lock().unlock();
    }

    bool initialize(const char* fileName, heaptrack_callback_t initBeforeCallback,
                    heaptrack_callback_initialized_t initAfterCallback, heaptrack_callback_t stopCallback)
    {
        debugLog<MinimalOutput>("initializing: %s", fileName);
        if (s_data) {
            debugLog<MinimalOutput>("%s", "already initialized");
            return true;
        }

        if (initBeforeCallback) {
            debugLog<MinimalOutput>("%s", "calling initBeforeCallback");
            initBeforeCallback();
            debugLog<MinimalOutput>("%s", "done calling initBeforeCallback");
        }

        // do some once-only initializations
        static std::once_flag once;
        std::call_once(once, [] {
            debugLog<MinimalOutput>("%s", "doing once-only initialization");

            Trace::setup();

            // do not trace forked child processes
            // TODO: make this configurable
            pthread_atfork(&prepare_fork, &parent_fork, &child_fork);

#ifdef __APPLE__
            _dyld_register_func_for_remove_image(&dyldImageRemoved);
            _dyld_register_func_for_add_image(&dyldImageAdded);
#endif

            atexit([]() {
                if (s_forceCleanup) {
                    return;
                }
                debugLog<MinimalOutput>("%s", "atexit()");

                // free internal libstdc++ resources
                // see also Valgrind's `--run-cxx-freeres` option
#ifdef __GLIBCXX__
                if (&__gnu_cxx::__freeres) {
                    __gnu_cxx::__freeres();
                }
#endif

                s_atexit.store(true);
                heaptrack_stop();
            });
        });

        const auto out = createFile(fileName);

        if (out == -1) {
            if (stopCallback) {
                stopCallback();
            }
            return false;
        }

        s_data = new LockedData(out, stopCallback);
        s_moduleCacheDirty.store(true, memory_order_relaxed);

        writeVersion();
        writeExe();
        writeCommandLine();
        writeSystemInfo();
        writeSuppressions();

        if (initAfterCallback) {
            debugLog<MinimalOutput>("%s", "calling initAfterCallback");
            initAfterCallback(s_data->out);
            debugLog<MinimalOutput>("%s", "calling initAfterCallback done");
        }

        debugLog<MinimalOutput>("%s", "initialization done");
        return true;
    }

    void shutdown()
    {
        if (!s_data) {
            return;
        }

        debugLog<MinimalOutput>("%s", "shutdown()");

        writeTimestamp();
        writeRSS();

        s_data->out.flush();
        s_data->out.close();

        // NOTE: we leak heaptrack data on exit, intentionally
        // This way, we can be sure to get all static deallocations.
        if (!s_atexit || s_forceCleanup) {
            delete s_data;
            s_data = nullptr;
        }

        debugLog<MinimalOutput>("%s", "shutdown() done");
    }

    void invalidateModuleCache()
    {
        if (!s_data) {
            return;
        }
        s_moduleCacheDirty.store(true, memory_order_relaxed);
    }

    void writeTimestamp()
    {
        if (!s_data || !s_data->out.canWrite()) {
            return;
        }

        auto elapsed = elapsedTime();

        debugLog<VeryVerboseOutput>("writeTimestamp(%" PRIx64 ")", elapsed.count());

        s_data->out.writeHexLine('c', static_cast<size_t>(elapsed.count()));
    }

    void writeRSS()
    {
        if (!s_data || !s_data->out.canWrite()) {
            return;
        }

        size_t rss = 0;

#ifdef __linux__
        if (s_data->procStatm == -1) {
            return;
        }
        // read RSS in pages from statm, then rewind for next read
        // NOTE: don't use fscanf here, it could potentially deadlock us
        const int BUF_SIZE = 512;
        char buf[BUF_SIZE + 1];
        if (read(s_data->procStatm, buf, BUF_SIZE) <= 0) {
            fprintf(stderr, "WARNING: Failed to read RSS value from /proc/self/statm.\n");
            close(s_data->procStatm);
            s_data->procStatm = -1;
            return;
        }
        lseek(s_data->procStatm, 0, SEEK_SET);

        if (sscanf(buf, "%*u %zu", &rss) != 1) {
            fprintf(stderr, "WARNING: Failed to read RSS value from /proc/self/statm.\n");
            close(s_data->procStatm);
            s_data->procStatm = -1;
            return;
        }
#elif defined(__FreeBSD__)
        auto proc_info = kinfo_getproc(getpid());
        if (proc_info == nullptr) {
            return;
        }

        rss = proc_info->ki_rssize;

        free(proc_info);
#elif defined(__APPLE__)
        mach_task_basic_info_data_t taskInfo = {};
        mach_msg_type_number_t taskInfoCount = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&taskInfo), &taskInfoCount)
            != KERN_SUCCESS) {
            return;
        }

        const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        if (!pageSize) {
            return;
        }
        rss = static_cast<size_t>(taskInfo.resident_size) / pageSize;
#endif

        // TODO: compare to rusage.ru_maxrss (getrusage) to find "real" peak?
        // TODO: use custom allocators with known page sizes to prevent tainting
        //       the RSS numbers with heaptrack-internal data

        s_data->out.writeHexLine('R', rss);
    }

    void writeVersion()
    {
        s_data->out.writeHexLine('v', static_cast<size_t>(HEAPTRACK_VERSION),
                                 static_cast<size_t>(HEAPTRACK_FILE_FORMAT_VERSION));
    }

    void writeExe()
    {
        const int BUF_SIZE = 1023;
        char buf[BUF_SIZE + 1];
        size_t size = 0;

#ifdef __linux__
        const auto bytesRead = readlink("/proc/self/exe", buf, BUF_SIZE);
        if (bytesRead > 0) {
            size = static_cast<size_t>(bytesRead);
        }
#elif defined(__FreeBSD__)
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        size = BUF_SIZE;
        sysctl(mib, 4, buf, &size, NULL, 0);
#elif defined(__APPLE__)
        uint32_t bufferSize = sizeof(buf);
        if (_NSGetExecutablePath(buf, &bufferSize) == 0) {
            size = strlen(buf);
        }
#endif

        if (size > 0 && size < BUF_SIZE) {
            buf[size] = 0;
            s_data->out.write("x %x %s\n", size, buf);
        }
    }

    void writeCommandLine()
    {
        s_data->out.write("X");
#ifdef __APPLE__
        const auto argc = *_NSGetArgc();
        const auto argv = *_NSGetArgv();
        for (int i = 0; i < argc; ++i) {
            s_data->out.write(" %s", argv[i]);
        }
#else
        const int BUF_SIZE = 4096;
        char buf[BUF_SIZE + 1] = {0};

#ifdef __linux__
        auto fd = open("/proc/self/cmdline", O_RDONLY);
        auto bytesRead = read(fd, buf, BUF_SIZE);
        close(fd);
#elif defined(__FreeBSD__)
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, getpid()};
        size_t bytesRead = BUF_SIZE;
        sysctl(mib, 4, buf, &bytesRead, NULL, 0);
#endif

        char* end = buf + bytesRead;
        for (char* p = buf; p < end;) {
            s_data->out.write(" %s", p);
            while (*p++)
                ; // skip until start of next 0-terminated section
        }
#endif

        s_data->out.write("\n");
    }

    void writeSystemInfo()
    {
        s_data->out.writeHexLine('I', static_cast<size_t>(sysconf(_SC_PAGESIZE)),
                                 static_cast<size_t>(sysconf(_SC_PHYS_PAGES)));
    }

    void writeSuppressions()
    {
#ifdef __APPLE__
        return;
#else
        if (!__lsan_default_suppressions)
            return;

        const char* suppressions = __lsan_default_suppressions();
        if (!suppressions)
            return;

        std::istringstream stream(suppressions);
        std::string line;
        while (std::getline(stream, line)) {
            s_data->out.write("S ");
            s_data->out.write(line);
            s_data->out.write("\n");
        }
#endif
    }

    void handleMalloc(void* ptr, size_t size, const Trace& trace)
    {
        if (!s_data || !s_data->out.canWrite()) {
            return;
        }
        updateModuleCache();

        const auto index = s_data->traceTree.index(trace, [](uintptr_t ip, uint32_t index) {
            // decrement addresses by one - otherwise we misattribute the cost to the wrong instruction
            // for some reason, it seems like we always get the instruction _after_ the one we are interested in
            // see also: https://github.com/libunwind/libunwind/issues/287
            // and https://bugs.kde.org/show_bug.cgi?id=439897
            --ip;

            return s_data->out.writeHexLine('t', ip, index);
        });

#ifdef DEBUG_MALLOC_PTRS
        auto it = s_data->known.find(ptr);
        assert(it == s_data->known.end());
        s_data->known.insert(ptr);
#endif

        s_data->out.writeHexLine('+', size, index, reinterpret_cast<uintptr_t>(ptr));
    }

    void handleFree(void* ptr)
    {
        if (!s_data || !s_data->out.canWrite()) {
            return;
        }

#ifdef DEBUG_MALLOC_PTRS
        auto it = s_data->known.find(ptr);
        assert(it != s_data->known.end());
        s_data->known.erase(it);
#endif

        s_data->out.writeHexLine('-', reinterpret_cast<uintptr_t>(ptr));
    }

    static bool isPaused()
    {
        return s_paused;
    }

    static void setPaused(bool state)
    {
        s_paused = state;
    }

private:
#ifndef __APPLE__
    static int dl_iterate_phdr_callback(struct dl_phdr_info* info, size_t /*size*/, void* data)
    {
        auto heaptrack = reinterpret_cast<HeapTrack*>(data);
        const char* fileName = info->dlpi_name;
        if (!fileName || !fileName[0]) {
            fileName = "x";
        }

        debugLog<VerboseOutput>("dlopen_notify_callback: %s %zx", fileName, info->dlpi_addr);

        if (!heaptrack->s_data->out.write("m %x %s %zx -", strlen(fileName), fileName, info->dlpi_addr)) {
            return 1;
        }

        for (int i = 0; i < info->dlpi_phnum; i++) {
            const auto& phdr = info->dlpi_phdr[i];
            if (phdr.p_type == PT_LOAD) {
                if (!heaptrack->s_data->out.write(" %zx %zx", phdr.p_vaddr, phdr.p_memsz)) {
                    return 1;
                }
            }
        }

        if (!heaptrack->s_data->out.write("\n")) {
            return 1;
        }

        return 0;
    }
#else
    static void dyldImageAdded(const mach_header* header, intptr_t slide)
    {
        moduleCache().addImage(header, slide);
        s_moduleCacheDirty.store(true, memory_order_relaxed);
    }

    static void dyldImageRemoved(const mach_header* header, intptr_t /*slide*/)
    {
        moduleCache().removeImage(header);
        s_moduleCacheDirty.store(true, memory_order_relaxed);
    }

    static bool writeMachModule(const MachModuleCache::Module& module, void* context)
    {
        auto* heaptrack = static_cast<HeapTrack*>(context);
        if (!heaptrack->s_data->out.write("m %x %s %zx %s", strlen(module.fileName), module.fileName, module.slide,
                                          module.uuid)) {
            return false;
        }
        if (module.textSize && !heaptrack->s_data->out.write(" %zx %zx", module.textAddress, module.textSize)) {
            return false;
        }
        return heaptrack->s_data->out.write("\n");
    }

    bool writeMachModules()
    {
        return moduleCache().forEach(&writeMachModule, this);
    }
#endif

    static void prepare_fork()
    {
        debugLog<MinimalOutput>("%s", "prepare_fork()");
        // don't do any custom malloc handling while inside fork
        RecursionGuard::setActive(true);
    }

    static void parent_fork()
    {
        debugLog<MinimalOutput>("%s", "parent_fork()");
        // the parent process can now continue its custom malloc tracking
        RecursionGuard::setActive(false);
    }

    static void child_fork()
    {
        debugLog<MinimalOutput>("%s", "child_fork()");
        // but the forked child process cleans up itself
        // this is important to prevent two processes writing to the same file
        s_data = nullptr;
        RecursionGuard::setActive(true);
    }

    void updateModuleCache()
    {
        if (!s_data || !s_data->out.canWrite() || !s_moduleCacheDirty.exchange(false, memory_order_relaxed)) {
            return;
        }
        debugLog<MinimalOutput>("%s", "updateModuleCache()");
        if (!s_data->out.write("m 1 -\n")) {
            s_moduleCacheDirty.store(true, memory_order_relaxed);
            return;
        }
#ifdef __APPLE__
        if (!writeMachModules()) {
            s_moduleCacheDirty.store(true, memory_order_relaxed);
        }
#else
        if (dl_iterate_phdr(&dl_iterate_phdr_callback, this) != 0) {
            s_moduleCacheDirty.store(true, memory_order_relaxed);
        }
#endif
    }

    void writeError()
    {
        debugLog<MinimalOutput>("write error %d/%s", errno, strerror(errno));
        printBacktrace();
        shutdown();
    }

    class LockStatus
    {
    public:
        LockStatus(bool isLocked)
            : isLocked(isLocked)
        {
        }
        explicit operator bool() const
        {
            return isLocked;
        }

    private:
        bool isLocked = false;
    };

    /**
     * To prevent deadlocks on shutdown, we try to lock from the timer thread
     *
     * TODO: c++17 return std::optional<HeapTrack>
     */
    template <typename StopLockCheck>
    static LockStatus tryLock(StopLockCheck stopLockCheck)
    {
        debugLog<VeryVerboseOutput>("%s", "trying to acquire lock");
        while (!lock().try_lock()) {
            if (stopLockCheck()) {
                return false;
            }
            this_thread::sleep_for(chrono::microseconds(1));
        }
        debugLog<VeryVerboseOutput>("%s", "lock acquired");
        return true;
    }

    /**
     * Create locked HeapTrack instance after a successful call to tryLock
     */
    HeapTrack(POTENTIALLY_UNUSED LockStatus lockStatus)
    {
        assert(lockStatus);
    }

    struct LockedData
    {
        LockedData(int out, heaptrack_callback_t stopCallback)
            : out(out)
            , stopCallback(stopCallback)
        {

            debugLog<MinimalOutput>("%s", "constructing LockedData");
#ifdef __linux__
            procStatm = open("/proc/self/statm", O_RDONLY);
            if (procStatm == -1) {
                fprintf(stderr, "WARNING: Failed to open /proc/self/statm for reading: %s.\n", strerror(errno));
            }
#endif

            // ensure this utility thread is not handling any signals
            // our host application may assume only one specific thread
            // will handle the threads, if that's not the case things
            // seemingly break in non-obvious ways.
            // see also: https://bugs.kde.org/show_bug.cgi?id=378494
            sigset_t previousMask;
            sigset_t newMask;
            sigfillset(&newMask);
            if (pthread_sigmask(SIG_SETMASK, &newMask, &previousMask) != 0) {
                fprintf(stderr, "WARNING: Failed to block signals, disabling timer thread.\n");
                return;
            }

            // the mask we set above will be inherited by the thread that we spawn below
            timerThread = std::thread([&]() {
                RecursionGuard::setActive(true);
                debugLog<MinimalOutput>("%s", "timer thread started");

                // now loop and repeatedly print the timestamp and RSS usage to the data stream
                while (!stopTimerThread) {
                    // TODO: make interval customizable
                    this_thread::sleep_for(chrono::milliseconds(10));

                    const auto locked = tryLock([&] { return stopTimerThread.load(); });
                    if (!locked) {
                        break;
                    }

                    HeapTrack heaptrack(locked);
                    heaptrack.writeTimestamp();
                    heaptrack.writeRSS();
                }
            });

            // now restore the previous mask as if nothing ever happened
            if (pthread_sigmask(SIG_SETMASK, &previousMask, nullptr) != 0) {
                fprintf(stderr, "WARNING: Failed to restore the signal mask.\n");
            }
        }

        ~LockedData()
        {
            debugLog<MinimalOutput>("%s", "destroying LockedData");
            stopTimerThread = true;
            if (timerThread.joinable()) {
                try {
                    timerThread.join();
                } catch (const std::system_error&) {
                }
            }

            out.close();

#ifdef __linux__
            if (procStatm != -1) {
                close(procStatm);
            }
#endif

            if (stopCallback && (!s_atexit || s_forceCleanup)) {
                stopCallback();
            }
            debugLog<MinimalOutput>("%s", "done destroying LockedData");
        }

        LineWriter out;

#ifdef __linux__
        /// /proc/self/statm file descriptor to read RSS value from
        int procStatm = -1;
#endif

        TraceTree traceTree;

        atomic<bool> stopTimerThread {false};
        std::thread timerThread;

        heaptrack_callback_t stopCallback = nullptr;

#ifdef DEBUG_MALLOC_PTRS
        tsl::robin_set<void*> known;
#endif
    };

    static LockedData* s_data;
    static std::atomic<bool> s_moduleCacheDirty;

private:
    static std::mutex& lock()
    {
        static std::mutex instance;
        return instance;
    }

#ifdef __APPLE__
    static MachModuleCache& moduleCache()
    {
        static MachModuleCache instance;
        return instance;
    }
#endif

    static std::atomic<bool> s_paused;
};

HeapTrack::LockedData* HeapTrack::s_data {nullptr};
std::atomic<bool> HeapTrack::s_moduleCacheDirty {true};
std::atomic<bool> HeapTrack::s_paused {false};
}

static void heaptrack_realloc_impl(void* ptr_in, size_t size, void* ptr_out)
{
    if (!HeapTrack::isPaused() && ptr_out && !RecursionGuard::isActive()) {
        RecursionGuard guard;

        debugLog<VeryVerboseOutput>("heaptrack_realloc(%p, %zu, %p)", ptr_in, size, ptr_out);

        Trace trace;
        trace.fill(2 + HEAPTRACK_DEBUG_BUILD * 3);

        HeapTrack::op(guard, [&](HeapTrack& heaptrack) {
            if (ptr_in) {
                heaptrack.handleFree(ptr_in);
            }
            heaptrack.handleMalloc(ptr_out, size, trace);
        });
    }
}

extern "C" {

bool heaptrack_init(const char* outputFileName, heaptrack_callback_t initBeforeCallback,
                    heaptrack_callback_initialized_t initAfterCallback, heaptrack_callback_t stopCallback)
{
    RecursionGuard guard;
    // initialize
    startTime();
    s_forceCleanup.store(false);

    debugLog<MinimalOutput>("heaptrack_init(%s)", outputFileName);

    bool initialized = false;
    const auto ret = HeapTrack::op(guard, [&](HeapTrack& heaptrack) {
        initialized = heaptrack.initialize(outputFileName, initBeforeCallback, initAfterCallback, stopCallback);
    });
    assert(ret);
    return ret && initialized;
}

void heaptrack_stop()
{
    RecursionGuard guard;

    debugLog<MinimalOutput>("%s", "heaptrack_stop()");

    assert(!s_forceCleanup);
    POTENTIALLY_UNUSED auto ret = HeapTrack::op(guard, [&](HeapTrack& heaptrack) {
        if (!s_atexit) {
            s_forceCleanup.store(true);
        }

        heaptrack.shutdown();
    });
    assert(ret);
}

void heaptrack_pause()
{
    HeapTrack::setPaused(true);
}

void heaptrack_resume()
{
    HeapTrack::setPaused(false);
}

void heaptrack_malloc(void* ptr, size_t size)
{
    if (!HeapTrack::isPaused() && ptr && !RecursionGuard::isActive()) {
        RecursionGuard guard;

        debugLog<VeryVerboseOutput>("heaptrack_malloc(%p, %zu)", ptr, size);

        Trace trace;
        trace.fill(2 + HEAPTRACK_DEBUG_BUILD * 2);

        HeapTrack::op(guard, [&](HeapTrack& heaptrack) { heaptrack.handleMalloc(ptr, size, trace); });
    }
}

void heaptrack_free(void* ptr)
{
    if (!HeapTrack::isPaused() && ptr && !RecursionGuard::isActive()) {
        RecursionGuard guard;

        debugLog<VeryVerboseOutput>("heaptrack_free(%p)", ptr);

        HeapTrack::op(guard, [&](HeapTrack& heaptrack) { heaptrack.handleFree(ptr); });
    }
}

void heaptrack_realloc(void* ptr_in, size_t size, void* ptr_out)
{
    heaptrack_realloc_impl(ptr_in, size, ptr_out);
}

void heaptrack_realloc2(uintptr_t ptr_in, size_t size, uintptr_t ptr_out)
{
    heaptrack_realloc_impl(reinterpret_cast<void*>(ptr_in), size, reinterpret_cast<void*>(ptr_out));
}

void* heaptrack_realloc_locked(void* ptr_in, size_t size, heaptrack_realloc_callback_t callback, void* context)
{
    if (!callback) {
        return nullptr;
    }
    if (HeapTrack::isPaused() || RecursionGuard::isActive()) {
        return callback(ptr_in, size, context);
    }

    RecursionGuard guard;
    Trace trace;
    trace.fill(2 + HEAPTRACK_DEBUG_BUILD * 3);

    void* ptr_out = nullptr;
    bool callbackCalled = false;
    const auto recorded = HeapTrack::op(guard, [&](HeapTrack& heaptrack) {
        ptr_out = callback(ptr_in, size, context);
        callbackCalled = true;
        if (!ptr_out) {
            return;
        }
        if (ptr_in) {
            heaptrack.handleFree(ptr_in);
        }
        heaptrack.handleMalloc(ptr_out, size, trace);
    });
    if (!recorded && !callbackCalled) {
        ptr_out = callback(ptr_in, size, context);
    }
    return ptr_out;
}

void heaptrack_invalidate_module_cache(heaptrack_invalidate_module_cache_callback callback)
{
    RecursionGuard guard;

    debugLog<VerboseOutput>("%s", "heaptrack_invalidate_module_cache()");

    HeapTrack::op(guard, [&](HeapTrack& heaptrack) {
        heaptrack.invalidateModuleCache();
        if (callback)
            callback();
    });
}

void heaptrack_warning(heaptrack_warning_callback_t callback)
{
    RecursionGuard guard;

    debugLog<WarningOutput>(callback);
}
}
