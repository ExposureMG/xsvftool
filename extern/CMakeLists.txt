cmake_minimum_required(VERSION 3.20)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# libusb — always built from source (needed by libftdi on non-Windows)
# ---------------------------------------------------------------------------
add_subdirectory(libusb)

if(WIN32)
    target_sources(usb-1.0 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os/windows_usbdk.c")
    include(CheckCSourceCompiles)
    check_c_source_compiles("#include <time.h>
int main(void) { struct timespec ts; return sizeof(ts) == 0; }"
        LIBUSB_HAVE_STRUCT_TIMESPEC)
    if(LIBUSB_HAVE_STRUCT_TIMESPEC)
        target_compile_definitions(usb-1.0 PRIVATE HAVE_STRUCT_TIMESPEC=1)
    endif()
endif()

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
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/arm64/FTD2XXstatic.lib")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_FTD2XX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftd2xx-win64")
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/Static/amd64/ftd2xx.lib")
    else()
        set(_FTD2XX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ftd2xx-win32")
        set(_FTD2XX_LIB "${_FTD2XX_DIR}/Static/i386/ftd2xx.lib")
    endif()

    add_library(ftd2xx STATIC IMPORTED GLOBAL)
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
