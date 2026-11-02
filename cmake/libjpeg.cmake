# libjpeg provider: a parent project (chisel) links one libjpeg for everything and defines
# JPEG::JPEG before adding this directory. only when it doesn't, a pinned static
# libjpeg-turbo is built here: libjpeg-turbo and mozjpeg export the same jpeg_* symbols,
# so a binary can't carry both.

set(JPEGULTRASCAN_LIBJPEG_TURBO_VERSION 3.2.0)
set(JPEGULTRASCAN_LIBJPEG_TURBO_SHA256 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e)
set(JPEGULTRASCAN_LIBJPEG_TURBO_URL
    "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${JPEGULTRASCAN_LIBJPEG_TURBO_VERSION}/libjpeg-turbo-${JPEGULTRASCAN_LIBJPEG_TURBO_VERSION}.tar.gz"
    CACHE STRING "libjpeg-turbo source archive, a URL or a local path")

if(NOT TARGET JPEG::JPEG)
    include(ExternalProject)
    set(_jus_prefix "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo")
    if(MSVC)
        set(_jus_lib "${_jus_prefix}/lib/jpeg-static.lib")
    else()
        set(_jus_lib "${_jus_prefix}/lib/libjpeg.a")
    endif()

    # ExternalProject doesn't inherit the toolchain; forward what cross builds need
    # (-m32 flags, macOS architectures, compilers)
    set(_jus_args
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_INSTALL_INCLUDEDIR=include
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DENABLE_SHARED=0
        -DENABLE_STATIC=1
        -DWITH_TURBOJPEG=0
        -DWITH_TOOLS=0
        -DWITH_TESTS=0
        -DWITH_FUZZ=0
        # simd changes speed only, never the bytes; without it no platform needs nasm
        -DWITH_SIMD=0
        "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
        "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}")
    if(CMAKE_C_COMPILER)
        list(APPEND _jus_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    if(CMAKE_BUILD_TYPE)
        list(APPEND _jus_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    elseif(NOT CMAKE_CONFIGURATION_TYPES)
        list(APPEND _jus_args -DCMAKE_BUILD_TYPE=Release)
    endif()
    if(APPLE)
        # a universal build lists several architectures; ExternalProject splits lists on "|"
        string(REPLACE ";" "|" _jus_archs "${CMAKE_OSX_ARCHITECTURES}")
        list(APPEND _jus_args
            "-DCMAKE_OSX_ARCHITECTURES=${_jus_archs}"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _jus_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(VCPKG_TARGET_TRIPLET)
        list(APPEND _jus_args "-DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}")
    endif()

    set(_jus_download)
    if(POLICY CMP0135)
        set(_jus_download DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    ExternalProject_Add(jpegultrascan_libjpeg_turbo
        URL "${JPEGULTRASCAN_LIBJPEG_TURBO_URL}"
        URL_HASH SHA256=${JPEGULTRASCAN_LIBJPEG_TURBO_SHA256}
        ${_jus_download}
        PREFIX "${_jus_prefix}/ep"
        INSTALL_DIR "${_jus_prefix}"
        LIST_SEPARATOR |
        CMAKE_ARGS ${_jus_args}
        BUILD_BYPRODUCTS "${_jus_lib}"
        UPDATE_DISCONNECTED 1)

    # an imported target's include directory has to exist at configure time
    file(MAKE_DIRECTORY "${_jus_prefix}/include")
    add_library(jpegultrascan_libjpeg STATIC IMPORTED GLOBAL)
    set_target_properties(jpegultrascan_libjpeg PROPERTIES
        IMPORTED_LOCATION "${_jus_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_jus_prefix}/include")
    add_dependencies(jpegultrascan_libjpeg jpegultrascan_libjpeg_turbo)
    add_library(JPEG::JPEG ALIAS jpegultrascan_libjpeg)
    set(JPEGULTRASCAN_LIBJPEG "libjpeg-turbo ${JPEGULTRASCAN_LIBJPEG_TURBO_VERSION} (built here)")
else()
    set(JPEGULTRASCAN_LIBJPEG "JPEG::JPEG from the parent project")
endif()

# mozjpeg's defaults turn on its own scan search and progression, so the encoder has to
# select its libjpeg-turbo profile first. its jpeglib.h is the only reliable tell: the
# version macros overlap with libjpeg-turbo's, and jconfig.h may not exist yet when a
# parent builds mozjpeg as an ExternalProject.
set(JPEGULTRASCAN_MOZJPEG AUTO CACHE STRING "Whether JPEG::JPEG is mozjpeg: AUTO, ON or OFF")
set_property(CACHE JPEGULTRASCAN_MOZJPEG PROPERTY STRINGS AUTO ON OFF)
if(JPEGULTRASCAN_MOZJPEG STREQUAL "AUTO")
    set(_jus_target JPEG::JPEG)
    get_target_property(_jus_aliased JPEG::JPEG ALIASED_TARGET)
    if(_jus_aliased)
        set(_jus_target ${_jus_aliased})
    endif()
    get_target_property(_jus_dirs ${_jus_target} INTERFACE_INCLUDE_DIRECTORIES)
    set(JPEGULTRASCAN_IS_MOZJPEG OFF)
    if(_jus_dirs)
        foreach(_jus_dir IN LISTS _jus_dirs)
            if(EXISTS "${_jus_dir}/jpeglib.h")
                file(STRINGS "${_jus_dir}/jpeglib.h" _jus_hit REGEX "JCP_FASTEST")
                if(_jus_hit)
                    set(JPEGULTRASCAN_IS_MOZJPEG ON)
                endif()
            endif()
        endforeach()
    endif()
else()
    set(JPEGULTRASCAN_IS_MOZJPEG ${JPEGULTRASCAN_MOZJPEG})
endif()
message(STATUS "jpegultrascan: libjpeg is ${JPEGULTRASCAN_LIBJPEG}, mozjpeg: ${JPEGULTRASCAN_IS_MOZJPEG}")
