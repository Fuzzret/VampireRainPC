cmake_minimum_required(VERSION 3.20)
project(VampireRainRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Build with Clang (clang-cl on Windows). The recompiled code uses Clang builtins and attributes.")
endif()

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp_out" CACHE PATH "XenonRecomp output folder")
set(XENONRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../XenonRecomp" CACHE PATH "XenonRecomp source folder (provides simde)")
set(VR_ARCH "x86-64-v3" CACHE STRING "CPU level for the generated code (x86-64-v2 for CPUs without AVX2)")

if(NOT EXISTS "${RECOMP_DIR}/ppc_recomp_shared.h")
    message(FATAL_ERROR "No generated code in ${RECOMP_DIR}. Run XenonRecomp first, or set -DRECOMP_DIR=...")
endif()
if(NOT EXISTS "${XENONRECOMP_DIR}/thirdparty/simde/x86/avx.h")
    message(FATAL_ERROR "simde not found under ${XENONRECOMP_DIR}/thirdparty. Set -DXENONRECOMP_DIR=...")
endif()

# ---------------------------------------------------------------------------
# Generated code
# ---------------------------------------------------------------------------
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${RECOMP_DIR}/ppc_recomp.*.cpp")
list(APPEND RECOMP_SOURCES "${RECOMP_DIR}/ppc_func_mapping.cpp")

add_library(guest_code STATIC ${RECOMP_SOURCES})
target_include_directories(guest_code PUBLIC "${RECOMP_DIR}" "${XENONRECOMP_DIR}/thirdparty/simde")

set(GUEST_CHECKS "${CMAKE_CURRENT_SOURCE_DIR}/src/guest_checks.h")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl. Don't rely on CMake's default flags (they aren't always
    # applied): exceptions are needed for ExTerminateThread, and the generated
    # code must always be optimized.
    target_compile_options(guest_code PUBLIC "/clang:-march=${VR_ARCH}" /EHsc)
    target_compile_options(guest_code PRIVATE /clang:-fno-strict-aliasing -Wno-everything "/FI${GUEST_CHECKS}" /O2)
else()
    target_compile_options(guest_code PUBLIC "-march=${VR_ARCH}")
    target_compile_options(guest_code PRIVATE -fno-strict-aliasing -w -include "${GUEST_CHECKS}"
                                              $<$<CONFIG:Debug>:-O2>)
endif()

# ---------------------------------------------------------------------------
# XMA audio decoding: the parts of FFmpeg Xenia uses (Xenia's FFmpeg fork,
# which adds an XMA frame decoder; LGPL). Built from its source in a Xenia
# checkout; without it the game runs silently.
# ---------------------------------------------------------------------------
set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../xenia/third_party/FFmpeg" CACHE PATH "Xenia's FFmpeg submodule")
if(EXISTS "${FFMPEG_DIR}/libavcodec/wmaprodec.c" AND EXISTS "${FFMPEG_DIR}/config_windows_x86_64.h")
    if(NOT CMAKE_C_COMPILER)
        set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}")
    endif()
    enable_language(C)
    set(FFMPEG_SOURCES
    libavutil/adler32.c libavutil/aes.c libavutil/aes_ctr.c libavutil/audio_fifo.c libavutil/avstring.c
    libavutil/avsscanf.c libavutil/base64.c libavutil/blowfish.c libavutil/bprint.c libavutil/buffer.c
    libavutil/cast5.c libavutil/camellia.c libavutil/channel_layout.c libavutil/color_utils.c libavutil/cpu.c
    libavutil/crc.c libavutil/des.c libavutil/dict.c libavutil/display.c libavutil/dovi_meta.c
    libavutil/downmix_info.c libavutil/encryption_info.c libavutil/error.c libavutil/eval.c libavutil/fifo.c
    libavutil/file.c libavutil/file_open.c libavutil/float_dsp.c libavutil/fixed_dsp.c libavutil/frame.c
    libavutil/hash.c libavutil/hdr_dynamic_metadata.c libavutil/hmac.c libavutil/hwcontext.c
    libavutil/imgutils.c libavutil/integer.c libavutil/intmath.c libavutil/lfg.c libavutil/lls.c
    libavutil/log.c libavutil/log2_tab.c libavutil/mathematics.c libavutil/mastering_display_metadata.c
    libavutil/md5.c libavutil/mem.c libavutil/murmur3.c libavutil/opt.c libavutil/parseutils.c
    libavutil/pixdesc.c libavutil/pixelutils.c libavutil/random_seed.c libavutil/rational.c
    libavutil/reverse.c libavutil/rc4.c libavutil/ripemd.c libavutil/samplefmt.c libavutil/sha.c
    libavutil/sha512.c libavutil/slicethread.c libavutil/spherical.c libavutil/stereo3d.c
    libavutil/threadmessage.c libavutil/time.c libavutil/timecode.c libavutil/tree.c libavutil/twofish.c
    libavutil/utils.c libavutil/xga_font_data.c libavutil/xtea.c libavutil/tea.c libavutil/tx.c
    libavutil/tx_float.c libavutil/tx_double.c libavutil/tx_int32.c libavutil/video_enc_params.c
    libavutil/film_grain_params.c libavutil/x86/cpu.c libavutil/x86/fixed_dsp_init.c
    libavutil/x86/float_dsp_init.c libavutil/x86/imgutils_init.c libavutil/x86/lls_init.c
    libavcodec/ac3_parser.c libavcodec/adts_parser.c libavcodec/allcodecs.c libavcodec/avcodec.c
    libavcodec/avdct.c libavcodec/avpacket.c libavcodec/avpicture.c libavcodec/bitstream.c
    libavcodec/bitstream_filter.c libavcodec/bitstream_filters.c libavcodec/bsf.c libavcodec/codec_desc.c
    libavcodec/codec_par.c libavcodec/d3d11va.c libavcodec/decode.c libavcodec/dirac.c libavcodec/dv_profile.c
    libavcodec/encode.c libavcodec/imgconvert.c libavcodec/jni.c libavcodec/mathtables.c
    libavcodec/mediacodec.c libavcodec/mpeg12framerate.c libavcodec/options.c libavcodec/parser.c
    libavcodec/parsers.c libavcodec/profiles.c libavcodec/qsv_api.c libavcodec/raw.c libavcodec/utils.c
    libavcodec/vorbis_parser.c libavcodec/xiph.c libavcodec/faandct.c libavcodec/faanidct.c
    libavcodec/fdctdsp.c libavcodec/jfdctfst.c libavcodec/jfdctint.c libavcodec/idctdsp.c
    libavcodec/simple_idct.c libavcodec/jrevdct.c libavcodec/mdct_float.c libavcodec/mdct_fixed_32.c
    libavcodec/sinewin.c libavcodec/wma_freqs.c libavcodec/wmaprodec.c libavcodec/wma.c
    libavcodec/wma_common.c libavcodec/null_bsf.c libavcodec/pthread.c libavcodec/pthread_slice.c
    libavcodec/pthread_frame.c libavcodec/avfft.c libavcodec/fft_float.c libavcodec/fft_fixed_32.c
    libavcodec/fft_init_table.c libavcodec/file_open.c libavcodec/x86/constants.c
    libavcodec/x86/fdctdsp_init.c libavcodec/x86/fft_init.c libavcodec/x86/idctdsp_init.c
    libavcodec/x86/fdct.c)
    list(TRANSFORM FFMPEG_SOURCES PREPEND "${FFMPEG_DIR}/")
    add_library(ffmpeg_xma STATIC ${FFMPEG_SOURCES})
    target_include_directories(ffmpeg_xma PUBLIC "${FFMPEG_DIR}")
    target_compile_definitions(ffmpeg_xma PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
    if(WIN32)
        target_include_directories(ffmpeg_xma BEFORE PRIVATE "${FFMPEG_DIR}/compat/atomics/win32")
        target_link_libraries(ffmpeg_xma PUBLIC bcrypt)
    else()
        target_include_directories(ffmpeg_xma BEFORE PRIVATE "${FFMPEG_DIR}/compat/atomics/gcc")
    endif()
    # Always optimized: FFmpeg relies on dead-code elimination of disabled features.
    if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
        target_compile_options(ffmpeg_xma PRIVATE /O2 /w)
    else()
        target_compile_options(ffmpeg_xma PRIVATE -O2 -w)
    endif()
    set(VR_HAVE_FFMPEG ON)
else()
    message(STATUS "Xenia's FFmpeg not found at '${FFMPEG_DIR}' (set -DFFMPEG_DIR=...); building without game audio")
    set(VR_HAVE_FFMPEG OFF)
endif()

# ---------------------------------------------------------------------------
# Runtime
# ---------------------------------------------------------------------------
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/*.cpp")

# GPU definitions and the shader interpreter, vendored from Xenia (BSD license,
# see third_party/xenia/LICENSE).
set(XENIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenia")
set(XENIA_SOURCES
    "${XENIA_DIR}/xenia/gpu/ucode.cc"
    "${XENIA_DIR}/xenia/gpu/xenos.cc"
    "${XENIA_DIR}/xenia/gpu/register_file.cc"
    "${XENIA_DIR}/xenia/gpu/shader_interpreter.cc")

add_executable(VampireRain ${RUNTIME_SOURCES} ${XENIA_SOURCES})
target_include_directories(VampireRain PRIVATE "${XENIA_DIR}")
target_link_libraries(VampireRain PRIVATE guest_code)
if(VR_HAVE_FFMPEG)
    target_link_libraries(VampireRain PRIVATE ffmpeg_xma)
    target_compile_definitions(VampireRain PRIVATE VR_HAVE_FFMPEG)
endif()

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(VampireRain PRIVATE /EHsc $<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

if(WIN32)
    target_compile_definitions(VampireRain PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(VampireRain PRIVATE xinput9_1_0 user32 gdi32 d3d12 dxgi xaudio2 ole32 winmm windowscodecs)
    # The DXC shader compiler (much faster than the built-in fxc) is loaded
    # from beside the exe; copy it from the Windows SDK if the build prompt
    # knows where that is. Without it the runtime falls back to fxc.
    set(VR_SDK_BIN "$ENV{WindowsSdkVerBinPath}x64")
    if(EXISTS "${VR_SDK_BIN}/dxcompiler.dll" AND EXISTS "${VR_SDK_BIN}/dxil.dll")
        add_custom_command(TARGET VampireRain POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VR_SDK_BIN}/dxcompiler.dll" "${VR_SDK_BIN}/dxil.dll" "$<TARGET_FILE_DIR:VampireRain>")
    else()
        message(STATUS "dxcompiler.dll/dxil.dll not found in '${VR_SDK_BIN}'; shaders will compile with fxc")
    endif()
    # Program icon: the game's own, which build.ps1 saves from your copy of
    # the executable (vr_tool icon) - it isn't part of this repository.
    get_filename_component(VR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../work/icon.ico" ABSOLUTE)
    if(EXISTS "${VR_ICON}")
        include(CheckLanguage)
        check_language(RC)
        if(CMAKE_RC_COMPILER)
            enable_language(RC)
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" "1 ICON \"${VR_ICON}\"\n")
            target_sources(VampireRain PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc")
            set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/icon.rc" PROPERTIES OBJECT_DEPENDS "${VR_ICON}")
        endif()
    endif()
    # A normal Windows program: no console window (start with --console to
    # get one), still entering at main().
    set_target_properties(VampireRain PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(VampireRain PRIVATE /ENTRY:mainCRTStartup)
    # Put the game (and the shader compiler DLLs) next to the game files, so
    # it starts with a double-click on VampireRain.exe in the project folder.
    option(VR_COPY_TO_ROOT "Copy VampireRain.exe to the project folder after building" ON)
    if(VR_COPY_TO_ROOT)
        set(VR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
        add_custom_command(TARGET VampireRain POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:VampireRain>" "${VR_ROOT}")
        if(EXISTS "${VR_SDK_BIN}/dxcompiler.dll" AND EXISTS "${VR_SDK_BIN}/dxil.dll")
            add_custom_command(TARGET VampireRain POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VR_SDK_BIN}/dxcompiler.dll" "${VR_SDK_BIN}/dxil.dll" "${VR_ROOT}")
        endif()
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(VampireRain PRIVATE Threads::Threads)
endif()
