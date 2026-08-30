# heaptrack: bash script to inject/preload
configure_file(heaptrack.sh.cmake
    ${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}/heaptrack @ONLY
)

install(PROGRAMS ${PROJECT_BINARY_DIR}/${BIN_INSTALL_DIR}/heaptrack
    DESTINATION ${BIN_INSTALL_DIR}
)

if (APPLE)
    add_library(heaptrack_unwind STATIC trace_macos.cpp)
elseif (HEAPTRACK_USE_LIBUNWIND)
    add_library(heaptrack_unwind STATIC trace_libunwind.cpp)
    target_include_directories(heaptrack_unwind PRIVATE ${LIBUNWIND_INCLUDE_DIRS})
    target_link_libraries(heaptrack_unwind PRIVATE ${LIBUNWIND_LIBRARIES})
else()
    add_library(heaptrack_unwind STATIC trace_unwind_tables.cpp)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(LIBUTIL_LIBRARY "util")
endif()

if (NOT APPLE)
    set(HEAPTRACK_RT_LIBRARY "rt")
endif()

set_property(TARGET heaptrack_unwind PROPERTY POSITION_INDEPENDENT_CODE ON)

# heaptrack_env: runtime environment tests
add_executable(heaptrack_env heaptrack_env.cpp)

install(TARGETS heaptrack_env
    RUNTIME DESTINATION ${LIBEXEC_INSTALL_DIR}
)

set_target_properties(heaptrack_env PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIBEXEC_INSTALL_DIR}"
)

# heaptrack_preload: track a newly started process
if (APPLE)
    set(HEAPTRACK_PRELOAD_LIBRARY_TYPE SHARED)
    set(HEAPTRACK_PLATFORM_SOURCES machmodulecache.cpp)
    set(HEAPTRACK_PRELOAD_SOURCE heaptrack_interpose.cpp)
else()
    set(HEAPTRACK_PRELOAD_LIBRARY_TYPE MODULE)
    set(HEAPTRACK_PRELOAD_SOURCE heaptrack_preload.cpp)
endif()

add_library(heaptrack_preload ${HEAPTRACK_PRELOAD_LIBRARY_TYPE}
    ${HEAPTRACK_PRELOAD_SOURCE}
    libheaptrack.cpp
    ${HEAPTRACK_PLATFORM_SOURCES}
)

target_link_libraries(heaptrack_preload LINK_PRIVATE
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
    ${LIBUTIL_LIBRARY}
    heaptrack_unwind
    ${HEAPTRACK_RT_LIBRARY}
)

set_target_properties(heaptrack_preload PROPERTIES
    VERSION ${HEAPTRACK_LIB_VERSION}
    SOVERSION ${HEAPTRACK_LIB_SOVERSION}
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIB_INSTALL_DIR}/heaptrack"
)

install(TARGETS heaptrack_preload
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}/heaptrack/
)

if (NOT APPLE)
    # heaptrack_inject: track an already running process
    add_library(heaptrack_inject MODULE
        heaptrack_inject.cpp
        libheaptrack.cpp
    )

    target_link_libraries(heaptrack_inject PRIVATE
        ${CMAKE_DL_LIBS}
        ${CMAKE_THREAD_LIBS_INIT}
        ${LIBUTIL_LIBRARY}
        heaptrack_unwind
        ${HEAPTRACK_RT_LIBRARY}
        tsl::robin_map
    )

    set_target_properties(heaptrack_inject PROPERTIES
        VERSION ${HEAPTRACK_LIB_VERSION}
        SOVERSION ${HEAPTRACK_LIB_SOVERSION}
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/${LIB_INSTALL_DIR}/heaptrack"
    )

    install(TARGETS heaptrack_inject
        LIBRARY DESTINATION ${LIB_INSTALL_DIR}/heaptrack/
    )
endif()

# public API for custom pool allocators or static binaries
install(FILES heaptrack_api.h
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include
)
