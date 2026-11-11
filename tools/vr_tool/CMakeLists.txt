cmake_minimum_required(VERSION 3.20)
project(vr_tool LANGUAGES C CXX)

# Disc extraction and XEX decryption for build.ps1. Uses the XEX, AES and LZX
# code from a XenonRecomp checkout.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(XENONRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/XenonRecomp" CACHE PATH "XenonRecomp source folder")

set(X "${XENONRECOMP_DIR}")
if(NOT EXISTS "${X}/XenonUtils/xex.cpp")
    message(FATAL_ERROR "XenonRecomp not found at ${X} (set -DXENONRECOMP_DIR=...)")
endif()

add_executable(vr_tool
    vr_tool.cpp
    "${X}/XenonUtils/xex.cpp"
    "${X}/XenonUtils/image.cpp"
    "${X}/XenonUtils/xex_patcher.cpp"
    "${X}/XenonUtils/memory_mapped_file.cpp"
    "${X}/XenonUtils/xdbf_wrapper.cpp"
    "${X}/thirdparty/libmspack/libmspack/mspack/lzxd.c"
    "${X}/thirdparty/libmspack/libmspack/mspack/system.c"
    "${X}/thirdparty/tiny-AES-c/aes.c")
target_include_directories(vr_tool PRIVATE
    "${X}/XenonUtils"
    "${X}/thirdparty"
    "${X}/thirdparty/tiny-AES-c"
    "${X}/thirdparty/TinySHA1"
    "${X}/thirdparty/libmspack/libmspack/mspack"
    "${X}/thirdparty/fmt/include")
target_compile_definitions(vr_tool PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
    target_compile_options(vr_tool PRIVATE /EHsc -Wno-everything)
else()
    target_compile_options(vr_tool PRIVATE -w)
endif()
