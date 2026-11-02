# stands in for chisel in the CI: builds mozjpeg as an ExternalProject from sources present
# at configure time (chisel has them as a submodule), defines JPEG::JPEG the way chisel's
# top-level CMakeLists.txt does, then adds this project as a subdirectory
cmake_minimum_required(VERSION 3.16...4.4)
project(jpegultrascan_mozjpeg_parent LANGUAGES C CXX)

if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(_moz_src ${CMAKE_BINARY_DIR}/mozjpeg-4.1.5)
if(NOT EXISTS ${_moz_src}/jpeglib.h)
    file(DOWNLOAD https://github.com/mozilla/mozjpeg/archive/refs/tags/v4.1.5.tar.gz
         ${CMAKE_BINARY_DIR}/mozjpeg-4.1.5.tar.gz
         EXPECTED_HASH SHA256=9fcbb7171f6ac383f5b391175d6fb3acde5e64c4c4727274eade84ed0998fcc1)
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/mozjpeg-4.1.5.tar.gz DESTINATION ${CMAKE_BINARY_DIR})
endif()

include(ExternalProject)
set(_moz_build ${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build)
if(MSVC)
    set(_mozjpeg_lib "${_moz_build}/Release/jpeg-static.lib")
else()
    set(_mozjpeg_lib "${_moz_build}/libjpeg.a")
endif()
ExternalProject_Add(mozjpeg
    SOURCE_DIR ${_moz_src}
    PREFIX ${CMAKE_BINARY_DIR}/mozjpeg
    CMAKE_ARGS
        -DENABLE_SHARED=FALSE
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DPNG_SUPPORTED=0
        -DWITH_SIMD=0
        -DWITH_JAVA=0
        -DWITH_TURBOJPEG=0
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_mozjpeg_lib})
file(MAKE_DIRECTORY ${_moz_build})
add_library(mozjpeg_static STATIC IMPORTED GLOBAL)
set_target_properties(mozjpeg_static PROPERTIES
    IMPORTED_LOCATION ${_mozjpeg_lib}
    INTERFACE_INCLUDE_DIRECTORIES "${_moz_src};${_moz_build}")
add_dependencies(mozjpeg_static mozjpeg)
add_library(JPEG::JPEG ALIAS mozjpeg_static)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. jpegultrascancpp EXCLUDE_FROM_ALL)
