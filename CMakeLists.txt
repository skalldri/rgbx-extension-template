# rgbx extension: builds a device .llext, a simulator .wasm, and a device
# RGBX v2 .rgbx package from single translation units, using the rgbx-sdk
# pinned in cmake/fw-release.cmake.
#
# Ordering below is LOAD-BEARING: the SDK must be downloaded and
# CMAKE_TOOLCHAIN_FILE set BEFORE project(), because that is where CMake
# loads the toolchain. Everything SDK-related after project() is a plain
# include().
#
#   ./build.sh                      # build/arm + build/wasm + build/rgbx-v2
#   cmake --preset arm && cmake --build --preset arm    # one target
#
# To rename your extension, change the project() name below — the output
# files follow it. Keep it matching ^[a-z0-9_]{1,25}$ (it becomes the
# .llext filename on the device, and must equal your future registry-entry
# name in the rgb-sunglasses repo).
#
# Renaming also means editing "extensionId" in rgbx-v2.json: nothing derives
# it from the project name, so a rename alone ships a v2 package still
# calling itself my_extension. That field's shape is
# ^[a-z0-9][a-z0-9._-]{0,30}$, which unlike the project-name shape above
# refuses a leading underscore, so pick a name both accept.
#
# C or C++: the build uses src/main.cpp when it exists, else src/main.c
# (same preference as the firmware repo's own extension build). To go C++,
# copy examples/cpp-waves/main.cpp to src/main.cpp — see "Writing your
# extension in C++" in the README.

cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fw-release.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/get-rgbx-sdk.cmake")  # sets RGBX_SDK_DIR

if(NOT DEFINED RGBX_TARGET)
    message(FATAL_ERROR "configure via a preset: cmake --preset arm | cmake --preset wasm | cmake --preset rgbx-v2 (or pass -DRGBX_TARGET=...)")
endif()
# Spell out every target: a typo such as -DRGBX_TARGET=rgbx_v2 should say so
# rather than quietly building whichever one the fallback happened to name.
if(RGBX_TARGET STREQUAL "arm")
    set(CMAKE_TOOLCHAIN_FILE "${RGBX_SDK_DIR}/cmake/toolchains/arm-llext.cmake")
elseif(RGBX_TARGET STREQUAL "wasm")
    set(CMAKE_TOOLCHAIN_FILE "${RGBX_SDK_DIR}/cmake/toolchains/wasm.cmake")
elseif(RGBX_TARGET STREQUAL "rgbx-v2")
    set(CMAKE_TOOLCHAIN_FILE "${RGBX_SDK_DIR}/cmake/toolchains/rgbx-v2.cmake")
else()
    message(FATAL_ERROR "unknown RGBX_TARGET '${RGBX_TARGET}': expected arm, wasm or rgbx-v2")
endif()

project(my_extension C CXX)

include("${RGBX_SDK_DIR}/cmake/rgbx-sdk-config.cmake")

if(RGBX_TARGET STREQUAL "rgbx-v2")
    # The v2 guest is a different ABI, not a recompile of the .llext/.wasm
    # source: no framebuffer, no memory, no floating point. So it gets its own
    # translation unit, and its own manifest describing the package the
    # firmware installs. rgbx-v2.json names src/main_v2.c as the source it
    # covers, and the packager refuses to run unless that is the same file
    # this line compiled, so keep the two in step when renaming either.
    rgbx_add_extension(${PROJECT_NAME}
        SOURCES src/main_v2.c
        MANIFEST rgbx-v2.json)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    rgbx_add_extension(${PROJECT_NAME} SOURCES src/main.cpp)
else()
    rgbx_add_extension(${PROJECT_NAME} SOURCES src/main.c)
endif()
