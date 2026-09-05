cmake_minimum_required(VERSION 3.20)
project(xsvftool LANGUAGES C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# In-tree dependencies  (libusb always; ftd2xx imported on Windows;
#                        libftdi from source on non-Windows)

add_subdirectory(extern)


# Source lists

option(XSVFTOOL_BUILD_CLI "Build xsvftool command-line executables" ON)

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
    xsvftool/xsvftool.c
)

set(XSVFTOOL_HEADERS
    libxsvf/libxsvf.h
    xsvftool/xsvftool.h
)


# Helper function — apply common settings to a target
#   Avoids duplicating the whole block for library vs CLI

function(xsvftool_configure_target tgt)

    target_compile_definitions(${tgt} PRIVATE COMBINED_BUILD)

    # Public headers (consumers only need libxsvf.h and xsvftool.h)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libxsvf>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/xsvftool>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/libxsvf
            ${CMAKE_CURRENT_SOURCE_DIR}/xsvftool
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
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/libftdi/xsvftool
            ${CMAKE_CURRENT_BINARY_DIR}/extern/libftdi/xsvftool
        )
        target_link_libraries(${tgt} PRIVATE ftdi1-static usb-1.0 m)
    endif()

endfunction()


# Static library target

add_library(xsvftool STATIC ${XSVFTOOL_SOURCES} ${XSVFTOOL_HEADERS})
target_compile_definitions(xsvftool PUBLIC XSVFTOOL_NO_MAIN)
xsvftool_configure_target(xsvftool)


# CLI executables (matching build_old/Makefile.old outputs)

if(XSVFTOOL_BUILD_CLI)
    # 1. Combined tool (supports -A FTDI and -D DirtyJTAG)
    add_executable(xsvftool_cli ${XSVFTOOL_SOURCES} ${XSVFTOOL_HEADERS})
    xsvftool_configure_target(xsvftool_cli)
    set_target_properties(xsvftool_cli PROPERTIES OUTPUT_NAME xsvftool)

    # 2. Standalone FTDI tool (FTD2XX on Windows, libftdi elsewhere)
    if(WIN32)
        set(FTDI_STANDALONE_NAME "xsvftool-ftd2xx")
        set(FTDI_STANDALONE_SRC  "xsvftool/xsvfplay_ftd2xx.c")
    else()
        set(FTDI_STANDALONE_NAME "xsvftool-libftdi")
        set(FTDI_STANDALONE_SRC  "xsvftool/xsvfplay_libftdi.c")
    endif()

    add_executable(${FTDI_STANDALONE_NAME} ${LIBXSVF_SOURCES} ${FTDI_STANDALONE_SRC})
    target_include_directories(${FTDI_STANDALONE_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libxsvf
        ${CMAKE_CURRENT_SOURCE_DIR}/xsvftool
    )
    if(WIN32)
        target_compile_definitions(${FTDI_STANDALONE_NAME} PRIVATE FORWIN _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(${FTDI_STANDALONE_NAME} PRIVATE ftd2xx ws2_32)
    else()
        target_include_directories(${FTDI_STANDALONE_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/libftdi/xsvftool
            ${CMAKE_CURRENT_BINARY_DIR}/extern/libftdi/xsvftool
        )
        target_link_libraries(${FTDI_STANDALONE_NAME} PRIVATE ftdi1-static usb-1.0 m)
    endif()

    # 3. Standalone DirtyJTAG tool
    add_executable(xsvftool-dirtyjtag ${LIBXSVF_SOURCES} xsvftool/xsvfplay_dirtyjtag.c)
    target_include_directories(xsvftool-dirtyjtag PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libxsvf
        ${CMAKE_CURRENT_SOURCE_DIR}/xsvftool
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/libusb/libusb
        ${CMAKE_CURRENT_BINARY_DIR}/extern/libusb_config
    )
    target_compile_definitions(xsvftool-dirtyjtag PRIVATE DIRTYJTAG_STANDALONE)
    if(WIN32)
        target_compile_definitions(xsvftool-dirtyjtag PRIVATE FORWIN _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(xsvftool-dirtyjtag PRIVATE usb-1.0 ws2_32)
    else()
        target_link_libraries(xsvftool-dirtyjtag PRIVATE usb-1.0 m)
    endif()

    # 4. Copy runtime DLLs next to the executables (matches copy-dlls from Makefile.old)
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)" OR CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64" OR CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)")
            set(_FTD2XX_RUNTIME_DLL "${CMAKE_CURRENT_SOURCE_DIR}/extern/ftd2xx-winarm/arm64/FTD2XX.dll")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_FTD2XX_RUNTIME_DLL "${CMAKE_CURRENT_SOURCE_DIR}/extern/ftd2xx-win64/amd64/ftd2xx64.dll")
        else()
            set(_FTD2XX_RUNTIME_DLL "${CMAKE_CURRENT_SOURCE_DIR}/extern/ftd2xx-win32/i386/ftd2xx.dll")
        endif()

        add_custom_command(TARGET xsvftool_cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_FTD2XX_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:xsvftool_cli>/"
            COMMENT "Copying ftd2xx runtime DLL to output directory"
        )
    endif()
endif()


# Tests

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()
