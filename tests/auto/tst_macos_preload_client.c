/*
    SPDX-FileCopyrightText: 2026 Lee ByeongJun <lbj199874@gmail.com>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

static void report(const char* allocator, void* pointer)
{
    printf("%s=%p\n", allocator, pointer);
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
