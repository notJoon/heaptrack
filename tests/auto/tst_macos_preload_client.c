/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <dlfcn.h>
#include <errno.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void* stress_allocations(void* context)
{
    const time_t deadline = *(const time_t*)context;
    size_t size = 1;
    while (time(NULL) < deadline) {
        void* pointer = malloc(size);
        if (pointer == NULL) {
            return pointer;
        }
        *(volatile unsigned char*)pointer = 0;
        free(pointer);
        size = size % 1024 + 1;
    }
    return context;
}

static int test_stress(void)
{
    const time_t deadline = time(NULL) + 2;
    pthread_t threads[4];
    for (size_t i = 0; i < 4; ++i) {
        if (pthread_create(&threads[i], NULL, &stress_allocations, (void*)&deadline) != 0) {
            return EXIT_FAILURE;
        }
    }

    const pid_t child = fork();
    if (child == 0) {
        execl("/usr/bin/true", "true", NULL);
        _exit(EXIT_FAILURE);
    }
    if (child == -1) {
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < 2000; ++i) {
        void* module = dlopen(HEAPTRACK_TEST_DYLIB, RTLD_NOW | RTLD_LOCAL);
        if (module == NULL || dlclose(module) != 0) {
            return EXIT_FAILURE;
        }
    }

    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < 4; ++i) {
        void* result = NULL;
        if (pthread_join(threads[i], &result) != 0 || result == NULL) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

static void report(const char* allocator, void* pointer)
{
    printf("%s=%p\n", allocator, pointer);
}

static int report_allocation(const char* allocator, size_t size, void* pointer)
{
    if (pointer == NULL) {
        return 0;
    }
    printf("%s=%zx:%p\n", allocator, size, pointer);
    return 1;
}

static int test_darwin_allocators(void)
{
    const malloc_type_id_t type_id = 0;
    malloc_zone_t* zone = malloc_default_zone();
    void* pointer;

#define CHECK_ALLOCATION(name, size, expression)                                                                      \
    do {                                                                                                               \
        pointer = (expression);                                                                                        \
        if (!report_allocation(name, size, pointer))                                                                   \
            return EXIT_FAILURE;                                                                                       \
    } while (0)

    CHECK_ALLOCATION("zone_malloc", 0x13001, malloc_zone_malloc(zone, 0x13001));
    malloc_zone_free(zone, pointer);
    CHECK_ALLOCATION("zone_calloc", 0x13002, malloc_zone_calloc(zone, 2, 0x9801));
    malloc_zone_free(zone, pointer);
    CHECK_ALLOCATION("zone_realloc_input", 0x10, malloc_zone_malloc(zone, 0x10));
    CHECK_ALLOCATION("zone_realloc", 0x13003, malloc_zone_realloc(zone, pointer, 0x13003));
    malloc_zone_free(zone, pointer);
    CHECK_ALLOCATION("zone_valloc", 0x13004, malloc_zone_valloc(zone, 0x13004));
    malloc_zone_free(zone, pointer);
    CHECK_ALLOCATION("zone_memalign", 0x13005, malloc_zone_memalign(zone, 16, 0x13005));
    malloc_zone_free(zone, pointer);

    CHECK_ALLOCATION("type_malloc", 0x13006, malloc_type_malloc(0x13006, type_id));
    malloc_type_free(pointer, type_id);
    CHECK_ALLOCATION("type_calloc", 0x13008, malloc_type_calloc(2, 0x9804, type_id));
    malloc_type_free(pointer, type_id);
    CHECK_ALLOCATION("type_realloc_input", 0x10, malloc_type_malloc(0x10, type_id));
    CHECK_ALLOCATION("type_realloc", 0x13009, malloc_type_realloc(pointer, 0x13009, type_id));
    malloc_type_free(pointer, type_id);
    CHECK_ALLOCATION("type_valloc", 0x1300a, malloc_type_valloc(0x1300a, type_id));
    malloc_type_free(pointer, type_id);
    CHECK_ALLOCATION("type_aligned_alloc", 0x13010, malloc_type_aligned_alloc(16, 0x13010, type_id));
    malloc_type_free(pointer, type_id);
    if (malloc_type_posix_memalign(&pointer, 16, 0x1300b, type_id) != 0
        || !report_allocation("type_posix_memalign", 0x1300b, pointer)) {
        return EXIT_FAILURE;
    }
    malloc_type_free(pointer, type_id);

    CHECK_ALLOCATION("type_zone_malloc", 0x1300c, malloc_type_zone_malloc(zone, 0x1300c, type_id));
    malloc_type_zone_free(zone, pointer, type_id);
    CHECK_ALLOCATION("type_zone_calloc", 0x1300e, malloc_type_zone_calloc(zone, 2, 0x9807, type_id));
    malloc_type_zone_free(zone, pointer, type_id);
    CHECK_ALLOCATION("type_zone_realloc_input", 0x10, malloc_type_zone_malloc(zone, 0x10, type_id));
    CHECK_ALLOCATION("type_zone_realloc", 0x1300f, malloc_type_zone_realloc(zone, pointer, 0x1300f, type_id));
    malloc_type_zone_free(zone, pointer, type_id);
    CHECK_ALLOCATION("type_zone_valloc", 0x13011, malloc_type_zone_valloc(zone, 0x13011, type_id));
    malloc_type_zone_free(zone, pointer, type_id);
    CHECK_ALLOCATION("type_zone_memalign", 0x13012, malloc_type_zone_memalign(zone, 16, 0x13012, type_id));
    malloc_type_zone_free(zone, pointer, type_id);

#undef CHECK_ALLOCATION
    return EXIT_SUCCESS;
}

static int test_edge_cases(void)
{
    errno = EDOM;
    free(NULL);
    if (errno != EDOM) {
        return EXIT_FAILURE;
    }

    void* zero = malloc(0);
    report("malloc_zero", zero);
    free(zero);

    errno = 0;
    if (calloc(SIZE_MAX, 2) != NULL) {
        return EXIT_FAILURE;
    }

    void* pointer = malloc(0x23456);
    report("failed_realloc", pointer);
    if (realloc(pointer, SIZE_MAX) != NULL) {
        free(pointer);
        return EXIT_FAILURE;
    }
    free(pointer);
    return EXIT_SUCCESS;
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL);

    if (argc == 2 && strcmp(argv[1], "--edge-cases") == 0) {
        return test_edge_cases();
    }
    if (argc == 2 && strcmp(argv[1], "--darwin-allocators") == 0) {
        return test_darwin_allocators();
    }
    if (argc == 2 && strcmp(argv[1], "--stress") == 0) {
        return test_stress();
    }

    void* pointer = malloc(0x12341);
    report("malloc", pointer);
    errno = EDOM;
    free(pointer);
    if (errno != EDOM) {
        return EXIT_FAILURE;
    }

    pointer = calloc(1, 0x12342);
    report("calloc", pointer);
    free(pointer);

    pointer = malloc(16);
    pointer = realloc(pointer, 0x12343);
    report("realloc", pointer);
    free(pointer);

    pointer = valloc(0x12344);
    report("valloc", pointer);
    free(pointer);

    errno = ERANGE;
    if (posix_memalign(&pointer, 16, 0x12345) != 0 || errno != ERANGE) {
        return EXIT_FAILURE;
    }
    report("posix_memalign", pointer);
    free(pointer);

    pointer = aligned_alloc(16, 0x12350);
    report("aligned_alloc", pointer);
    free(pointer);

    if (argc == 2 && strcmp(argv[1], "--explicit-stop") == 0) {
        void (*stop)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "heaptrack_stop");
        if (!stop) {
            return EXIT_FAILURE;
        }
        stop();
    }

    return EXIT_SUCCESS;
}
