cmake_minimum_required(VERSION 3.20)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# libusb — built from source in-tree
# ---------------------------------------------------------------------------
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libusb_config/config.h"
"#ifndef LIBUSB_CONFIG_H
#define LIBUSB_CONFIG_H

#ifdef _WIN32
#define PLATFORM_WINDOWS 1
#define DEFAULT_VISIBILITY
#define PRINTF_FORMAT(a, b)
#define ENABLE_LOGGING 1
#else
#define PLATFORM_POSIX 1
#define _GNU_SOURCE 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_TIMERFD 1
#define HAVE_EVENTFD 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_NFDS_T 1
#define DEFAULT_VISIBILITY __attribute__((visibility(\"default\")))
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#define ENABLE_LOGGING 1
#endif

#endif
")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libusb_config/version_describe.h"
"#ifndef LIBUSB_DESCRIBE
#define LIBUSB_DESCRIBE \"\"
#endif
")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libusb_config/libusb-1.0/libusb.h"
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/libusb.h\"
")

set(LIBUSB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/descriptor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/hotplug.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/strerror.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/sync.c
)

if(WIN32)
    list(APPEND LIBUSB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/events_windows.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/threads_windows.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/windows_common.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/windows_winusb.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/windows_usbdk.c
    )
    add_library(usb-1.0 STATIC ${LIBUSB_SOURCES})
    target_link_libraries(usb-1.0 PRIVATE cfgmgr32)

    include(CheckCSourceCompiles)
    check_c_source_compiles("#include <time.h>
int main(void) { struct timespec ts; return sizeof(ts) == 0; }"
        LIBUSB_HAVE_STRUCT_TIMESPEC)
    if(LIBUSB_HAVE_STRUCT_TIMESPEC)
        target_compile_definitions(usb-1.0 PRIVATE HAVE_STRUCT_TIMESPEC=1)
    endif()
    if(MSVC)
        target_compile_definitions(usb-1.0 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
elseif(APPLE)
    list(APPEND LIBUSB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/events_posix.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/threads_posix.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/darwin_usb.c
    )
    add_library(usb-1.0 STATIC ${LIBUSB_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(usb-1.0 PRIVATE
        Threads::Threads
        "-framework CoreFoundation"
        "-framework IOKit"
        "-framework Security"
        ${CMAKE_DL_LIBS}
    )
else()
    list(APPEND LIBUSB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/events_posix.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/threads_posix.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/linux_usbfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/linux_netlink.c
    )
    add_library(usb-1.0 STATIC ${LIBUSB_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(usb-1.0 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

set_target_properties(usb-1.0 PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(usb-1.0
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb
        ${CMAKE_CURRENT_BINARY_DIR}/libusb_config
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libusb
        ${CMAKE_CURRENT_BINARY_DIR}/libusb_config
)

# Provide variables so libftdi uses the in-tree libusb
set(LIBUSB_FOUND TRUE CACHE BOOL "" FORCE)
set(LIBUSB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb" CACHE PATH "" FORCE)
set(LIBUSB_LIBRARIES usb-1.0 CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# Backend selection
#   Windows  → prebuilt FTD2XX (imported target: ftd2xx)
#   Others   → libftdi built from source (target: ftdi1-static)
# ---------------------------------------------------------------------------
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)" OR CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64" OR CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)")
        set(_FTD2XX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftd2xx-winarm")
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/arm64/FTD2XX.lib")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_FTD2XX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftd2xx-win64")
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/amd64/ftd2xx.lib")
    else()
        set(_FTD2XX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftd2xx-win32")
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/i386/ftd2xx.lib")
    endif()

    add_library(ftd2xx UNKNOWN IMPORTED GLOBAL)
    set_target_properties(ftd2xx PROPERTIES
        IMPORTED_LOCATION             "${_FTD2XX_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_FTD2XX_DIR}"
    )
else()
    set(STATICLIBS      ON  CACHE BOOL "" FORCE)
    set(BUILD_TESTS     OFF CACHE BOOL "" FORCE)
    set(DOCUMENTATION   OFF CACHE BOOL "" FORCE)
    set(EXAMPLES        OFF CACHE BOOL "" FORCE)
    set(FTDIPP          OFF CACHE BOOL "" FORCE)
    set(FTDI_EEPROM     OFF CACHE BOOL "" FORCE)
    set(PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)

    add_subdirectory(libftdi)
endif()
