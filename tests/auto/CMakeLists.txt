
set(CMAKE_BUILD_TYPE Debug)
configure_file(tst_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/tst_config.h @ONLY)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/track
)

if (Boost_FILESYSTEM_FOUND)
    if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        set(LIBUTIL_LIBRARY "util")
    endif()

    include_directories(
        ${Boost_INCLUDE_DIRS}
    )
endif()

# tst_inject isn't asan-safe, so include it before we enable sanitizers
add_subdirectory(no_asan)

if (ECM_FOUND)
    include(ECMEnableSanitizers)
endif()

if (APPLE)
    add_executable(tst_trace tst_trace.cpp)
else()
    add_executable(tst_trace tst_trace.cpp ../../src/interpret/dwarfdiecache.cpp ../../src/interpret/demangler.cpp)
endif()
set_target_properties(tst_trace PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}")
target_link_libraries(tst_trace
    PRIVATE
        heaptrack_unwind
        Threads::Threads
        tsl::robin_map
)
if (NOT APPLE)
    target_link_libraries(tst_trace PRIVATE ${LIBDW_LIBRARIES})
    target_include_directories(tst_trace PRIVATE ${LIBDW_INCLUDE_DIRS})
endif()

add_test(NAME tst_trace COMMAND tst_trace)

if (TARGET heaptrack_interpret)
    if (APPLE)
        set(HEAPTRACK_INTERPRET_EXPECTED heaptrack.test_sysroot.macos.expected)
    else()
        set(HEAPTRACK_INTERPRET_EXPECTED heaptrack.test_sysroot.expected)
    endif()
    configure_file(tst_heaptrack_interpret.cmake.sh ${CMAKE_CURRENT_BINARY_DIR}/tst_heaptrack_interpret.sh @ONLY)
    add_test(NAME tst_heaptrack_interpret COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tst_heaptrack_interpret.sh)

    if (APPLE)
        add_executable(tst_macos_symbolication_fixture tst_macos_symbolication_fixture.cpp)
        target_compile_options(tst_macos_symbolication_fixture PRIVATE -g)
        set_target_properties(tst_macos_symbolication_fixture PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}"
        )
        add_custom_command(TARGET tst_macos_symbolication_fixture POST_BUILD
            COMMAND /usr/bin/dsymutil $<TARGET_FILE:tst_macos_symbolication_fixture>
            COMMAND /usr/bin/strip -S $<TARGET_FILE:tst_macos_symbolication_fixture>
        )
        configure_file(tst_macos_symbolication.cmake.sh
            ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_symbolication.sh @ONLY)
        add_test(NAME tst_macos_symbolication
            COMMAND /bin/sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_symbolication.sh)
    endif()
endif()

if (APPLE)
    add_executable(tst_macos_preload_client tst_macos_preload_client.c)
    target_compile_definitions(tst_macos_preload_client PRIVATE
        HEAPTRACK_HAVE_MALLOC_TYPE=$<BOOL:${HEAPTRACK_HAVE_MALLOC_TYPE}>
        HEAPTRACK_TEST_DYLIB="$<TARGET_FILE:testlib_indirect>")
    add_dependencies(tst_macos_preload_client testlib_indirect)
    set_target_properties(tst_macos_preload_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}"
    )
    if (HEAPTRACK_HAVE_MALLOC_TYPE)
        set(HEAPTRACK_MACOS_ALLOCATOR_COUNT 19)
    else()
        set(HEAPTRACK_MACOS_ALLOCATOR_COUNT 6)
    endif()
    configure_file(tst_macos_preload.cmake.sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_preload.sh @ONLY)
    add_test(NAME tst_macos_preload COMMAND /bin/sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_preload.sh)
    configure_file(tst_macos_launcher.cmake.sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_launcher.sh @ONLY)
    add_test(NAME tst_macos_launcher COMMAND /bin/sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_launcher.sh)
    if (TARGET heaptrack_interpret AND TARGET heaptrack_print)
        configure_file(tst_macos_e2e.cmake.sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_e2e.sh @ONLY)
        add_test(NAME tst_macos_e2e COMMAND /bin/sh ${CMAKE_CURRENT_BINARY_DIR}/tst_macos_e2e.sh)
        set_tests_properties(tst_macos_e2e PROPERTIES TIMEOUT 30)
        add_dependencies(tst_macos_preload_client heaptrack_interpret heaptrack_print)
    endif()
    set_tests_properties(tst_macos_preload tst_macos_launcher PROPERTIES TIMEOUT 30)
    add_dependencies(tst_macos_preload_client heaptrack_preload heaptrack_env)
endif()

if (Boost_FILESYSTEM_FOUND)
    if (NOT APPLE)
        set(HEAPTRACK_TEST_RT_LIBRARY rt)
    endif()

    add_executable(tst_libheaptrack
        tst_libheaptrack.cpp
        ../../src/track/libheaptrack.cpp)
    if (APPLE)
        target_sources(tst_libheaptrack PRIVATE ../../src/track/machmodulecache.cpp)
        target_compile_definitions(tst_libheaptrack PRIVATE HEAPTRACK_TEST_DYLIB="$<TARGET_FILE:testlib_indirect>")
        add_dependencies(tst_libheaptrack testlib_indirect)
    endif()
    set_target_properties(tst_libheaptrack PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}")
    target_link_libraries(tst_libheaptrack
        LINK_PRIVATE
            ${CMAKE_DL_LIBS}
            ${CMAKE_THREAD_LIBS_INIT}
            ${LIBUTIL_LIBRARY}
            heaptrack_unwind
            ${HEAPTRACK_TEST_RT_LIBRARY}
            ${Boost_FILESYSTEM_LIBRARY}
    )
    add_test(NAME tst_libheaptrack COMMAND tst_libheaptrack)

    add_executable(tst_io tst_io.cpp)
    set_target_properties(tst_io PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}")
    target_link_libraries(tst_io
            ${Boost_FILESYSTEM_LIBRARY}
    )
    add_test(NAME tst_io COMMAND tst_io)

    if (TARGET heaptrack_gui_private)
        find_package(Qt6 ${QT_MIN_VERSION} CONFIG OPTIONAL_COMPONENTS Test)
        if (Qt6Test_FOUND)
            add_executable(tst_parser tst_parser.cpp)
            set_target_properties(tst_parser PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}")
            target_link_libraries(tst_parser
                    ${Boost_FILESYSTEM_LIBRARY}
                    heaptrack_gui_private
                    Qt6::Test
            )
            add_test(NAME tst_parser COMMAND tst_parser)
        endif()
    endif()
endif()
