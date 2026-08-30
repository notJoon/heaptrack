/*
    SPDX-FileCopyrightText: 2026 notJoon

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

/**
 * @brief A native macOS backtrace implementation.
 */

#include "trace.h"

#include <cstdint>
#include <cstdio>
#include <pthread.h>

#if defined(__x86_64__)
#include <unwind.h>
#endif

#if defined(__arm64e__)
#include <ptrauth.h>
#endif

namespace {

#if defined(__aarch64__)
struct Frame
{
    Frame* previous;
    void* returnAddress;
};

void* stripReturnAddress(void* address)
{
#if defined(__arm64e__)
    return ptrauth_strip(address, ptrauth_key_return_address);
#else
    return address;
#endif
}

bool isValidFrame(const Frame* frame, uintptr_t stackBottom, uintptr_t stackTop)
{
    const auto address = reinterpret_cast<uintptr_t>(frame);
    constexpr auto frameAlignment = 2 * sizeof(void*);

    return address >= stackBottom && address <= stackTop - sizeof(Frame) && address % frameAlignment == 0;
}
#elif defined(__x86_64__)
struct Backtrace
{
    void** data;
    int size;
};

_Unwind_Reason_Code unwindCallback(_Unwind_Context* context, void* argument)
{
    auto* trace = static_cast<Backtrace*>(argument);
    const auto instructionPointer = _Unwind_GetIP(context);
    if (instructionPointer && trace->size < Trace::MAX_SIZE) {
        trace->data[trace->size++] = reinterpret_cast<void*>(instructionPointer);
    }

    return trace->size == Trace::MAX_SIZE ? _URC_END_OF_STACK : _URC_NO_REASON;
}
#endif

}

void Trace::setup()
{
}

void Trace::print()
{
    Trace trace;
    trace.fill(1);
    for (auto instructionPointer : trace) {
        fprintf(stderr, "%p\n", instructionPointer);
    }
}

int Trace::unwind(void** data)
{
#if defined(__aarch64__)
    const auto stackTop = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    const auto stackSize = pthread_get_stacksize_np(pthread_self());
    if (!stackTop || stackSize > stackTop || stackSize < sizeof(Frame)) {
        return 0;
    }

    const auto stackBottom = stackTop - stackSize;
    auto* frame = static_cast<Frame*>(__builtin_frame_address(0));
    int size = 0;

    while (size < MAX_SIZE && isValidFrame(frame, stackBottom, stackTop)) {
        auto* returnAddress = stripReturnAddress(frame->returnAddress);
        if (!returnAddress) {
            break;
        }
        data[size++] = returnAddress;

        auto* previous = frame->previous;
        if (reinterpret_cast<uintptr_t>(previous) <= reinterpret_cast<uintptr_t>(frame)) {
            break;
        }
        frame = previous;
    }

    return size;
#elif defined(__x86_64__)
    Backtrace trace = {data, 0};
    _Unwind_Backtrace(unwindCallback, &trace);
    return trace.size;
#else
#error Unsupported macOS architecture
#endif
}
