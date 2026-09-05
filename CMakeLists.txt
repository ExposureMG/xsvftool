cmake_minimum_required(VERSION 3.20)
project(xsvftool LANGUAGES C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# In-tree dependencies  (libusb always; ftd2xx imported on Windows;
#                        libftdi from source on non-Windows)
# ---------------------------------------------------------------------------
add_subdirectory(extern)

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------
option(XSVFTOOL_BUILD_CLI "Build xsvftool command-line executable" OFF)

set(LIBXSVF_SOURCES
    libxsvf/memname.c
    libxsvf/play.c
    libxsvf/scan.c
    libxsvf/statename.c
    libxsvf/svf.c
    libxsvf/tap.c
    libxsvf/xsvf.c
)

set(XSVFTOOL_SOURCES
    ${LIBXSVF_SOURCES}
    src/xsvftool.c
)

set(XSVFTOOL_HEADERS
    libxsvf/libxsvf.h
    src/xsvftool.h
)

# ---------------------------------------------------------------------------
# Helper function — apply common settings to a target
#   Avoids duplicating the whole block for library vs CLI
# ---------------------------------------------------------------------------
function(xsvftool_configure_target tgt)

    target_compile_definitions(${tgt} PRIVATE COMBINED_BUILD)

    # Public headers (consumers only need libxsvf.h and xsvftool.h)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libxsvf>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/libxsvf
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            # libusb needed on all platforms for DirtyJTAG
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/libusb/libusb
    )

    # Platform-specific backend includes and links
    if(WIN32)
        # ftd2xx.h comes via the INTERFACE_INCLUDE_DIRECTORIES of the
        # imported ftd2xx target, so no manual /I needed here.
        target_compile_definitions(${tgt} PRIVATE
            FORWIN _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(${tgt} PRIVATE ftd2xx usb-1.0 ws2_32)
    else()
        target_include_directories(${tgt} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/libftdi/src
            ${CMAKE_CURRENT_BINARY_DIR}/extern/libftdi/src
        )
        target_link_libraries(${tgt} PRIVATE ftdi1-static usb-1.0 m)
    endif()

endfunction()

# ---------------------------------------------------------------------------
# Static library target  (for embedding in NandProMax etc.)
# ---------------------------------------------------------------------------
add_library(xsvftool STATIC ${XSVFTOOL_SOURCES} ${XSVFTOOL_HEADERS})
target_compile_definitions(xsvftool PUBLIC XSVFTOOL_NO_MAIN)
xsvftool_configure_target(xsvftool)

# ---------------------------------------------------------------------------
# Optional CLI executable
# ---------------------------------------------------------------------------
if(XSVFTOOL_BUILD_CLI)
    add_executable(xsvftool_cli ${XSVFTOOL_SOURCES} ${XSVFTOOL_HEADERS})
    xsvftool_configure_target(xsvftool_cli)
    set_target_properties(xsvftool_cli PROPERTIES OUTPUT_NAME xsvftool)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()
