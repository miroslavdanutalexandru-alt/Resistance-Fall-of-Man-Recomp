cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.11.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"      # yz_frontier_trace, used by the live draw engine
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Vulkan RSX backend is opt-in while it grows (stage V0: clear + present).
# It loads libvulkan at run time, so building needs only the Vulkan headers --
# no target-architecture libvulkan, which keeps cross-compiling simple.
option(PS3RECOMP_RSX_VULKAN "Build the Vulkan RSX backend (Linux)" OFF)
list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_vulkan_backend\\.c$")
if(PS3RECOMP_RSX_VULKAN)
    if(WIN32 OR APPLE)
        message(FATAL_ERROR "PS3RECOMP_RSX_VULKAN is Linux-only for now")
    endif()
    find_path(PS3RECOMP_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT PS3RECOMP_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Debian/Ubuntu: "
            "libvulkan-dev, Fedora: vulkan-headers -- or pass "
            "-DPS3RECOMP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
    endif()
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_vulkan_backend.c")
    # HLSL -> SPIR-V for guest programs: a stub unless glslang is found below.
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")
endif()

# The Metal backend is Objective-C and Apple-only, so it is added explicitly
# rather than globbed. It needs no platform #ifdef of its own as a result.
if(APPLE)
    enable_language(OBJC)
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m")
endif()
# The HLSL -> MSL translator is C++ (it drives glslang's C++ API) and compiles
# to a stub wherever the shader tools are absent, so it is part of the
# library everywhere. See the "Guest shaders on Metal" section below.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_msl.cpp")
# cellAtrac decodes with FFmpeg's ATRAC3plus decoder as extracted by PPSSPP
# (LGPL-2.1, kept separate in third_party/at3_standalone -- see its README).
file(GLOB AT3_SOURCES "third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")  # upstream code
list(APPEND HLE_SOURCES ${AT3_SOURCES})
# Host audio output stage (libs/audio/audio_out.cpp): ring buffer + SoundTouch
# time stretch, after RPCS3's cellAudio "time stretching". SoundTouch is
# LGPL-2.1, vendored unmodified in third_party/soundtouch (see its README),
# built with RPCS3's defines.
set(SOUNDTOUCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/soundtouch")
file(GLOB SOUNDTOUCH_SOURCES "${SOUNDTOUCH_DIR}/source/SoundTouch/*.cpp")
set_source_files_properties(${SOUNDTOUCH_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")  # upstream code
set_source_files_properties(${SOUNDTOUCH_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio/audio_out.cpp" PROPERTIES
    COMPILE_DEFINITIONS "ST_NO_EXCEPTION_HANDLING;USE_MULTICH_ALWAYS;SOUNDTOUCH_FLOAT_SAMPLES;SOUNDTOUCH_ALLOW_SSE"
    INCLUDE_DIRECTORIES "${SOUNDTOUCH_DIR}/include")
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio/audio_out.cpp" ${SOUNDTOUCH_SOURCES})

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), which MSVC cannot parse, and it must be compiled
#    *inside the game build* by clang so the tail call is guaranteed -- a plain
#    call leaks one resolver frame per guest loop iteration. A port adds it to
#    its own target; see templates/project/CMakeLists.txt.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
    # SDL3 gamepads (libs/input/pad_sdl3.c): the default controller backend,
    # XInput stays as the fallback (PAD_BACKEND=xinput). Static, vendored in
    # third_party/SDL3 (built from upstream SDL with /MD, see its VERSION.txt).
    set(PS3RECOMP_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3")
    if(EXISTS "${PS3RECOMP_SDL3_DIR}/lib/SDL3-static.lib")
        target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_SDL3_DIR}/include")
        target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_PAD_SDL3=1)
        target_link_libraries(ps3recomp_runtime PRIVATE
            "${PS3RECOMP_SDL3_DIR}/lib/SDL3-static.lib"
            kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid
            advapi32 setupapi shell32 hid mincore dinput8 cfgmgr32)
        message(STATUS "  SDL3:         vendored (gamepad backend enabled)")
    else()
        message(WARNING "SDL3 not found (${PS3RECOMP_SDL3_DIR}/lib/SDL3-static.lib): "
                        "controllers are XInput only, so a DualSense / DualShock 4 / Switch pad works only "
                        "through Steam Input or DS4Windows")
    endif()
else()
    # cellPad and cellAudio select their SDL2 backends unconditionally off
    # Windows (see PAD_BACKEND_SDL2 / AUDIO_BACKEND_SDL2), so SDL2 is required
    # here -- docs/BUILDING.md already lists it as the Linux/macOS dependency.
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        else()
            target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "  SDL2:         found (input + audio backends enabled)")
    else()
        message(FATAL_ERROR
            "SDL2 not found. It provides the cellPad and cellAudio backends on "
            "non-Windows hosts.\n"
            "  macOS:  brew install sdl2\n"
            "  Debian: apt install libsdl2-dev\n"
            "If SDL2 is installed in a non-standard prefix, pass "
            "-DCMAKE_PREFIX_PATH=<prefix>.")
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(APPLE)
    target_link_libraries(ps3recomp_runtime PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework AppKit"
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "  Metal:        RSX backend enabled (rsx_metal_backend.m)")
endif()

if(PS3RECOMP_RSX_VULKAN)
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    # PUBLIC: ps3recomp_host selects its backend on this definition.
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_RSX_VULKAN)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  Vulkan:       RSX backend enabled (rsx_vulkan_backend.c, headers in ${PS3RECOMP_VULKAN_INCLUDE_DIR})")
endif()

# cellVdec decodes H.264 / MPEG-2 through FFmpeg when this is on. libavcodec
# is loaded at run time (dlopen / LoadLibrary), never linked, so building needs
# only the FFmpeg headers, and a binary run where FFmpeg is absent -- or with
# PS3RECOMP_FFMPEG=0 -- keeps the callback-only decoders. FFmpeg stays a
# separate, user-supplied LGPL library. See libs/codec/ffmpeg_runtime.h.
option(PS3RECOMP_FFMPEG_RUNTIME "Decode media through FFmpeg loaded at run time (needs the FFmpeg headers)" OFF)
if(PS3RECOMP_FFMPEG_RUNTIME)
    find_path(PS3RECOMP_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
    if(NOT PS3RECOMP_FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR "FFmpeg headers not found. Debian/Ubuntu: "
            "libavcodec-dev, Fedora: ffmpeg-free-devel, MSYS2: mingw-w64-x86_64-ffmpeg "
            "-- or pass -DPS3RECOMP_FFMPEG_INCLUDE_DIR=<dir containing libavcodec/avcodec.h>")
    endif()
    # a path cut short at a space (an unquoted -D on the command line) otherwise only fails much later,
    # at compile time, as "'libavcodec/avcodec.h' file not found"
    if(NOT EXISTS "${PS3RECOMP_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
        message(FATAL_ERROR "PS3RECOMP_FFMPEG_INCLUDE_DIR=\"${PS3RECOMP_FFMPEG_INCLUDE_DIR}\" has no "
            "libavcodec/avcodec.h. If the folder name has spaces, quote it: "
            "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=\"<dir>\"")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
    target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  FFmpeg:       run-time loaded (headers in ${PS3RECOMP_FFMPEG_INCLUDE_DIR})")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Metal: HLSL -> SPIR-V -> MSL
# ---------------------------------------------------------------------------
# The RSX decompilers emit HLSL for the D3D12 backend. Metal cannot compile
# that, so libs/video/rsx_shader_msl.cpp lowers it with glslang (HLSL ->
# SPIR-V) and spirv-cross (SPIR-V -> MSL). Both are optional: without them the
# module compiles to a stub, rsx_hlsl_to_msl_available() says so, and the
# Metal backend stays on its fixed-function path. The translation has no GPU
# in it, so it is built and tested on Linux as well -- that is where the
# decompilers' HLSL meets glslang's front end on every push.
#
#   macOS:  brew install glslang spirv-cross
#   Debian: apt install glslang-dev glslang-tools spirv-tools \
#                       libspirv-cross-c-shared-dev
#           (glslang-tools only because the packaged CMake config declares
#           the standalone binary as an imported target and fails to load
#           without it)
set(PS3RECOMP_MSL_TRANSLATION OFF)
if(UNIX)
    find_package(glslang CONFIG QUIET)

    # spirv-cross's C API. Debian ships it as a shared library and Homebrew as
    # a static one, under different pkg-config names -- and pkg-config itself
    # is not a given on a Mac, so fall back to a plain header + library search.
    set(SPVC_OK OFF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPVC QUIET spirv-cross-c-shared)
        if(NOT SPVC_FOUND)
            pkg_check_modules(SPVC QUIET spirv-cross-c)
        endif()
        if(SPVC_FOUND)
            set(SPVC_INC     ${SPVC_INCLUDE_DIRS})
            set(SPVC_LIBS    ${SPVC_LINK_LIBRARIES})
            set(SPVC_LIBDIRS ${SPVC_LIBRARY_DIRS})
            set(SPVC_OK ON)
        endif()
    endif()
    if(NOT SPVC_OK)
        find_path(SPVC_INC_DIR spirv_cross_c.h PATH_SUFFIXES spirv_cross)
        find_library(SPVC_LIB NAMES spirv-cross-c-shared spirv-cross-c)
        if(SPVC_INC_DIR AND SPVC_LIB)
            set(SPVC_INC  ${SPVC_INC_DIR})
            set(SPVC_LIBS ${SPVC_LIB})
            get_filename_component(SPVC_LIBDIRS "${SPVC_LIB}" DIRECTORY)
            set(SPVC_OK ON)
        endif()
    endif()

    if(glslang_FOUND AND SPVC_OK
       AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_msl_translation INTERFACE)
        target_compile_definitions(ps3recomp_msl_translation INTERFACE
            PS3RECOMP_HAVE_MSL_TRANSLATION=1)
        target_include_directories(ps3recomp_msl_translation INTERFACE ${SPVC_INC})
        target_link_libraries(ps3recomp_msl_translation INTERFACE
            glslang::glslang glslang::SPIRV ${SPVC_LIBS})
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_msl_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        # A static spirv-cross C API (Homebrew's) needs the backends it was
        # built from, and neither pkg-config nor the library name says so:
        # pick up the static siblings next to it. The shared one carries
        # its own.
        if(SPVC_LIBS MATCHES "\\.a$")
            foreach(part msl glsl hlsl cpp reflect util core)
                find_library(SPVC_PART_${part} NAMES spirv-cross-${part}
                    HINTS ${SPVC_LIBDIRS} NO_DEFAULT_PATH)
                if(SPVC_PART_${part})
                    target_link_libraries(ps3recomp_msl_translation INTERFACE
                        ${SPVC_PART_${part}})
                endif()
            endforeach()
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_msl_translation)
        set(PS3RECOMP_MSL_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translated to MSL (glslang ${glslang_VERSION}, ${SPVC_LIBS})")
    else()
        message(STATUS "  Shaders:      glslang/spirv-cross not found; Metal backend stays fixed-function")
    endif()
endif()

# Guest shaders on Vulkan: HLSL -> SPIR-V. glslang only -- Vulkan consumes
# SPIR-V, so spirv-cross is not needed. Without glslang
# libs/video/rsx_shader_spirv.cpp is a stub and the Vulkan backend stays on
# its fixed-function path.
if(PS3RECOMP_RSX_VULKAN)
    if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp"
            PROPERTIES COMPILE_DEFINITIONS PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
        target_link_libraries(ps3recomp_runtime PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_runtime PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        set(PS3RECOMP_SPIRV_TRANSLATION ON)
        message(STATUS "  Vulkan:       guest programs available (glslang ${glslang_VERSION}); "
                       "opt in with PS3RECOMP_VK_GUEST_PROGRAMS=1")
    else()
        message(STATUS "  Vulkan:       glslang not found; guest programs unavailable")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/clang:-ffp-contract=off,-ffp-contract=off>
        # Explicit, because the per-config defaults cannot be trusted: this
        # tree's caches carry CMAKE_C_FLAGS_RELEASE="" and CMAKE_CXX_FLAGS_RELEASE="",
        # and clang-cl with no /O flag compiles at /Od -- a "Release" runtime
        # was unoptimised without a word. Duplicating a default -O2 is harmless.
        $<$<CONFIG:Release,RelWithDebInfo>:-O2>
    )
    # clang-cl reads -Wall as /Wall (= -Weverything): hundreds of MB of warnings
    # on a first build (issue R2#11). /W3 is its usual level.
    if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(ps3recomp_runtime PRIVATE /W3)
    else()
        target_compile_options(ps3recomp_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    # SSE4.1 (any x86-64 since ~2008): pshufb/blendv for spu_shufb, the
    # hottest SPU op in lifted audio code. Probed rather than assumed: recent
    # Apple clang rejects the flag outright for arm64 ("unsupported option
    # '-msse4.1'"), which stops the whole runtime from building there.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-msse4.1 PS3RECOMP_HAS_MSSE41)
    if(PS3RECOMP_HAS_MSSE41)
        target_compile_options(ps3recomp_runtime PRIVATE -msse4.1)
    endif()
elseif(MSVC)
    # Real cl.exe -- clang-cl reports its id as Clang and is handled above.
    #
    # This does not currently build. The runtime and the lifted output use GCC/
    # Clang builtins that cl has no equivalent for, and the failure lands as a
    # wall of errors in whichever file the build happens to reach first rather
    # than as anything that names the cause. Say it here instead.
    message(WARNING
        "Building with cl.exe (MSVC). This is not currently supported.\n"
        "  The runtime uses __atomic_* builtins, __int128 and __builtin_bswap*, "
        "and the lifted output uses __int128 for the PPC 64x64 multiplies. "
        "cl has none of them.\n"
        "  Use clang-cl instead -- it consumes the MSVC headers and libraries, "
        "so nothing else about a Visual Studio build changes:\n"
        "    cmake -S . -B build -G Ninja "
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
        "  See docs/BUILDING.md. Porting the intrinsics to cl is a welcome "
        "contribution; nothing else stands in the way.")
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    # dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
    # that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly,
    # so the C++ boot scaffold never noticed; a C file including the same
    # shim does not get it for free.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
    # cellFiber switches contexts with Windows fibers on Windows and ucontext
    # everywhere else. Apple deprecated the ucontext routines and gates both
    # them and the shape of ucontext_t on _XOPEN_SOURCE, so whether that path
    # works on arm64 is a question to answer rather than assume.
    add_subdirectory(tests/fiber_switch)
    # Syscall 14's address/flags classification as a plain unit test: the
    # check is pure, so the test links one runtime translation unit
    # (lv2_lock_line.c) and nothing else.
    add_subdirectory(tests/syscalls)
    # cellVdec through its guest API: the callback-only decoder always, and
    # real H.264 / MPEG-2 decode when FFmpeg is on (libs/codec/tests).
    add_subdirectory(libs/codec/tests)
    # cellGcm's FIFO walker without a backend: where GET_REPORT writes for
    # each report DMA context. Links the runtime like the POSIX host below.
    if(UNIX)
        add_subdirectory(tests/gcm_reports)
    endif()
    # Native hot paths (R1 port): SSE2 texture untile/convert and the streaming
    # upload-heap copy, byte for byte against the code they replace.
    add_executable(test_native_hot tests/test_native_hot.c
                   libs/video/rsx_texture_layout.c libs/video/rsx_native_hot.c)
    target_include_directories(test_native_hot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                               "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME native_hot COMMAND test_native_hot)
    # PS3_LOG_FIRST, the saturating counter behind the "first n times" log lines.
    add_executable(test_log_first tests/test_log_first.c)
    target_include_directories(test_log_first PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME log_first COMMAND test_log_first)
    # ps3_ftrace.h: trigger-only PS3_FTRACE stays disarmed (no hook cost, no buffer) until triggered.
    add_executable(test_ftrace tests/test_ftrace.c)
    target_include_directories(test_ftrace PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME ftrace_trigger COMMAND test_ftrace)
    # port_pc_extras.h: rumble / stick / hold-toggle / UI scale / SFO / mod order logic (shared with R2/R3).
    add_executable(test_pc_extras tests/test_pc_extras.c)
    target_include_directories(test_pc_extras PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME pc_extras COMMAND test_pc_extras)
    # port_pc_extras2.h: benchmark statistics / crosshair / Steam Deck / window position (shared with R2/R3).
    add_executable(test_pc_extras2 tests/test_pc_extras2.c)
    target_include_directories(test_pc_extras2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME pc_extras2 COMMAND test_pc_extras2)
    # port_hdr.h, the SDR -> HDR curve of the present pass (and its inverse for screenshots).
    add_executable(test_port_hdr tests/test_port_hdr.c)
    target_include_directories(test_port_hdr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME port_hdr COMMAND test_port_hdr)
    # port_latency.h: PORT_LATENCY (Reflex / Anti-Lag 2) selection, shared with the R2/R3 ports.
    add_executable(test_port_latency tests/test_port_latency.c)
    target_include_directories(test_port_latency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME port_latency COMMAND test_port_latency)
    # audio_decomb.h: libmixer TYPE2A delayed side copy kept out of the stereo fold (R1 #10 "tin can").
    add_executable(test_audio_decomb tests/test_audio_decomb.c)
    target_include_directories(test_audio_decomb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio")
    add_test(NAME audio_decomb COMMAND test_audio_decomb)
    # rsx_vp_cpu.h: exact DLSS camera motion from a vertex program's matrices.
    add_executable(test_vp_cpu_camera tests/test_vp_cpu_camera.c)
    target_include_directories(test_vp_cpu_camera PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME vp_cpu_camera COMMAND test_vp_cpu_camera)
    # rsx_prim_assembly.h: strip / fan / quad streams continued across DRAW batches.
    add_executable(test_prim_assembly tests/test_prim_assembly.c)
    target_include_directories(test_prim_assembly PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME prim_assembly COMMAND test_prim_assembly)
    # rsx_ext_state.h: alpha-to-coverage, user clip planes, point sprites, guest samplers,
    # and the VS variant patch on real rsx_vp_decompiler output.
    add_executable(test_ext_state tests/test_ext_state.c libs/video/rsx_vp_decompiler.c)
    target_include_directories(test_ext_state PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME ext_state COMMAND test_ext_state)
    # savedata_backup.h: per-slot save backups (stamps, keep-newest-N rotation, identical-skip).
    add_executable(test_savedata_backup libs/system/tests/test_savedata_backup.c)
    add_test(NAME savedata_backup COMMAND test_savedata_backup "${CMAKE_CURRENT_BINARY_DIR}/savedata_backup_scratch")
    # port_frametime.h (the port's stutter recorder): avg / 1% low / 0.1% low, histogram, worst spikes.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../project/port_frametime.h")
        add_executable(test_frametime_stats tests/test_frametime_stats.cpp)
        target_include_directories(test_frametime_stats PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../project")
        add_test(NAME frametime_stats COMMAND test_frametime_stats)
    endif()
    # rfom_trophy_data.h (the port's trophy hook): unlocks recorded once, no repeated toasts,
    # platinum award, Saved Games record round trip.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../project/rfom_trophy_data.h")
        add_executable(test_trophy_toast tests/test_trophy_toast.cpp)
        target_include_directories(test_trophy_toast PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../project")
        add_test(NAME trophy_toast COMMAND test_trophy_toast WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    # rfom_achievements.h (the launcher's fan-made Achievements page): PROFILE.SAV decoding and rules.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../project/rfom_achievements.h")
        add_executable(test_rfom_achievements tests/test_rfom_achievements.cpp)
        target_include_directories(test_rfom_achievements PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../project")
        add_test(NAME rfom_achievements COMMAND test_rfom_achievements)
    endif()
    # port_ini_migrate.h (launchers + in-game settings loaders): renamed settings.ini keys, old -> new.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../project/tests/ini_migrate_test.cpp")
        add_executable(test_ini_migrate ../project/tests/ini_migrate_test.cpp)
        target_include_directories(test_ini_migrate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../project")
        target_compile_features(test_ini_migrate PRIVATE cxx_std_17)
        add_test(NAME ini_migrate COMMAND test_ini_migrate)
    endif()
    # spurs_mt.h: SPU lane pool sizing / CPU placement, and a threaded model of
    # the lane scheduler's targeted wakes (dependency order, no lost kicks).
    if(WIN32)
        add_executable(test_spurs_mt tests/test_spurs_mt.c)
        target_link_libraries(test_spurs_mt PRIVATE Synchronization)
        add_test(NAME spurs_mt COMMAND test_spurs_mt)
        set_tests_properties(spurs_mt PROPERTIES TIMEOUT 120)
        # rsx_texpack.inc's HD UI pack (RSX_HD_UI): lookup by the upload's key, staging of every level, fallbacks on
        # bad files, folder scan, memory budget, and a D3D12 WARP texture at the replacement's size.
        add_executable(test_hd_ui tests/test_hd_ui.c
                       libs/video/rsx_texture_layout.c libs/video/rsx_native_hot.c)
        target_include_directories(test_hd_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
        target_link_libraries(test_hd_ui PRIVATE d3d12 dxgi dxguid)
        add_test(NAME hd_ui COMMAND test_hd_ui WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        # rsx_fp_decompiler.c RSX_FP_EXTOPS: DDX/DDY/TXD/DST/LIT/STR/SFL/BEM/DP2A/RFL/TEXBEM emitted, ps_5_0
        # compiles, and LIT/DST/RFL/DP2A executed on WARP against reference values.
        add_executable(test_fp_extops tests/test_fp_extops.c libs/video/rsx_fp_decompiler.c)
        target_include_directories(test_fp_extops PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
        target_link_libraries(test_fp_extops PRIVATE d3d11 dxgi dxguid d3dcompiler)
        add_test(NAME fp_extops COMMAND test_fp_extops)
        # rsx_pso_cache.c (RSX_PSO_LIBRARY): the D3D12 pipeline library round trip on WARP, and damaged / truncated /
        # other GPU / other build / driver-rejected files ignored safely.
        add_executable(test_pso_cache tests/test_pso_cache.c libs/video/rsx_pso_cache.c)
        target_include_directories(test_pso_cache PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
        target_link_libraries(test_pso_cache PRIVATE d3d12 dxgi dxguid d3dcompiler)
        add_test(NAME pso_cache COMMAND test_pso_cache WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        # rsx_mt_record.c (RSX_MT_RECORD): the job queue and checksum memo under stress, and a heavy synthetic frame
        # stream on WARP rendered pixel-exact with the views created inline and on the worker pool. With an Agility
        # SDK bin folder (D3D12Core.dll + d3d12SDKLayers.dll) in RSX_D3D12_SDK_LAYERS_DIR it runs under the debug
        # layer with GPU-based validation. `test_mt_record <dir> timing` also prints the render-thread timings.
        # The folder's D3D12Core.dll version goes in RSX_D3D12_SDK_LAYERS_VER (the test exports it as D3D12SDKVersion);
        # the build copies the two DLLs next to the test in mtr_d3d12\.
        set(RSX_D3D12_SDK_LAYERS_DIR "" CACHE PATH "Agility SDK bin folder for the D3D12 debug layer in test_mt_record")
        set(RSX_D3D12_SDK_LAYERS_VER "620" CACHE STRING "Agility SDK version of RSX_D3D12_SDK_LAYERS_DIR")
        set(_mtr_sdk "${RSX_D3D12_SDK_LAYERS_DIR}")
        if(NOT _mtr_sdk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scratch/agility/pkg/build/native/bin/x64/d3d12SDKLayers.dll")
            set(_mtr_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../scratch/agility/pkg/build/native/bin/x64")
        endif()
        add_executable(test_mt_record tests/test_mt_record.c libs/video/rsx_mt_record.c libs/video/rsx_mt_replay.c)
        target_include_directories(test_mt_record PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
        target_link_libraries(test_mt_record PRIVATE d3d12 dxgi dxguid d3dcompiler)
        target_compile_definitions(test_mt_record PRIVATE MTR_AGILITY_VER=${RSX_D3D12_SDK_LAYERS_VER})
        if(_mtr_sdk)
            add_custom_command(TARGET test_mt_record POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:test_mt_record>/mtr_d3d12"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mtr_sdk}/D3D12Core.dll" "${_mtr_sdk}/d3d12SDKLayers.dll"
                        "$<TARGET_FILE_DIR:test_mt_record>/mtr_d3d12/")
            file(TO_NATIVE_PATH "${_mtr_sdk}" _mtr_sdk)
        else()
            set(_mtr_sdk "none")
        endif()
        add_test(NAME mt_record COMMAND test_mt_record "${_mtr_sdk}" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(mt_record PROPERTIES TIMEOUT 600)
        # HDUIPack.exe (project/hd_ui_pack.cpp + hd_ui_game_r1.h): DDS round trip through the renderer's check and
        # staging, atomic pack commit / recovery, atomic upscaler install, and every menu / HUD texture of the
        # installed game keyed as the upload keys it (skipped without the game files).
        get_filename_component(_hdui_repo "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
        if(EXISTS "${_hdui_repo}/project/hd_ui_pack.cpp" AND EXISTS "${_hdui_repo}/project/tests/hd_ui/hd_ui_test.cpp")
            add_executable(test_hd_ui_pack "${_hdui_repo}/project/tests/hd_ui/hd_ui_test.cpp"
                           libs/video/rsx_texture_layout.c libs/video/rsx_native_hot.c third_party/miniz/miniz.c)
            target_include_directories(test_hd_ui_pack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/libs/video" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb" "${_hdui_repo}/project")
            target_compile_features(test_hd_ui_pack PRIVATE cxx_std_17)
            target_compile_definitions(test_hd_ui_pack PRIVATE _CRT_SECURE_NO_WARNINGS HDUI_REPO="${_hdui_repo}")
            target_link_libraries(test_hd_ui_pack PRIVATE urlmon bcrypt)
            set(_hdui_usr "none")
            if(EXISTS "${_hdui_repo}/gamefiles/PS3_GAME/USRDIR/game.psarc")
                set(_hdui_usr "${_hdui_repo}/gamefiles/PS3_GAME/USRDIR")
            endif()
            add_test(NAME hd_ui_pack_dds COMMAND test_hd_ui_pack dds)
            add_test(NAME hd_ui_pack_folder COMMAND test_hd_ui_pack folder "${CMAKE_CURRENT_BINARY_DIR}/hd_ui_folder_test")
            add_test(NAME hd_ui_pack_upscaler COMMAND test_hd_ui_pack upscaler "${CMAKE_CURRENT_BINARY_DIR}/hd_ui_upscaler_test")
            # GPU safety without the GPU: the upscaler's command line, the watchdog (on a stand-in upscaler) and the
            # settings after a crashed / failed run
            add_test(NAME hd_ui_pack_gpu_safety COMMAND test_hd_ui_pack gpu_safety "${CMAKE_CURRENT_BINARY_DIR}/hd_ui_gpu_safety_test")
            set_tests_properties(hd_ui_pack_gpu_safety PROPERTIES TIMEOUT 120)
            # The pack and tools\upscaler on different drives (skipped when LOCALAPPDATA is on the repo's drive)
            set(_xvol "none")
            if(DEFINED ENV{LOCALAPPDATA})
                file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _la)
                string(SUBSTRING "${_la}" 0 2 _la_drive)
                string(SUBSTRING "${_hdui_repo}" 0 2 _repo_drive)
                string(TOUPPER "${_la_drive}" _la_drive)
                string(TOUPPER "${_repo_drive}" _repo_drive)
                if(NOT _la_drive STREQUAL _repo_drive)
                    set(_xvol "${_la}/hd_ui_xvolume_test")
                endif()
            endif()
            add_test(NAME hd_ui_pack_xvolume COMMAND test_hd_ui_pack xvolume "${CMAKE_CURRENT_BINARY_DIR}/hd_ui_xvolume_test" "${_xvol}")
            set_tests_properties(hd_ui_pack_xvolume PROPERTIES SKIP_RETURN_CODE 77)
            add_test(NAME hd_ui_pack_real COMMAND test_hd_ui_pack real "${_hdui_usr}")
            set_tests_properties(hd_ui_pack_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        endif()
    endif()

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spinlock_word_width
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spinlock_word_width.py")
        add_test(NAME spurs_task_exit_code_initialize
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_task_exit_code_initialize.py")
        add_test(NAME spurs_taskset_size
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_taskset_size.py")
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
    endif()
    if(NOT WIN32)
        find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME spu_fnms_zero_extended
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
        endif()
    endif()
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME loaded_image_memory
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loaded_image_memory.py")
        add_test(NAME loader_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loader_memory_domain.py")
        add_test(NAME mmapper_hle_heap
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_mmapper_hle_heap.py")
        add_test(NAME user_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_user_memory_domain.py")
    endif()
endif()

# ---------------------------------------------------------------------------
# POSIX host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. Builds on any POSIX host: it picks Metal on Apple and the null backend's
# headless software path everywhere else, so Linux -- which has no real backend
# yet -- can still run the whole guest command stream end to end in CI.
if(UNIX)
    add_executable(ps3recomp_host
        runtime/host/host_posix.c
        runtime/host/host_stubs.c
    )
    target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime)
    if(NOT WIN32)
        # glibc keeps the math functions in a separate library; MSVC does not.
        target_link_libraries(ps3recomp_host PRIVATE m)
    endif()
    target_include_directories(ps3recomp_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # The runtime carries C++ (and, with translation on, spirv-cross), so the
    # C++ driver must do the final link or the standard library goes missing.
    set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)

    # Guest shader translation, decompilers -> HLSL -> MSL, with no GPU: the
    # regression net for the Metal backend's shader path on every platform.
    if(PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_shader_msl
            libs/video/tests/test_shader_msl.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(test_shader_msl PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_msl PRIVATE ps3recomp_msl_translation)
    endif()

    # The HLSL -> SPIR-V translation cache, memory and disk. Needs glslang
    # only, no GPU.
    if(PS3RECOMP_SPIRV_TRANSLATION)
        add_executable(test_shader_spirv
            libs/video/tests/test_shader_spirv.c
            libs/video/rsx_shader_spirv.cpp
        )
        target_include_directories(test_shader_spirv PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_spirv PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(test_shader_spirv PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        set_target_properties(test_shader_spirv PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endif()

# ---------------------------------------------------------------------------
# PPU boot smoke title
# ---------------------------------------------------------------------------
# The one target that LINKS AND RUNS the PPU boot scaffold. runtime/ppu/ is the
# game-agnostic boot path every port is built on, and nothing here has ever
# built it: it #includes the lifter's per-game ppu_recomp.h, so
# tools/check_ppu_scaffold.py compiles it against a stub header and stops
# there. Compiling is not running, and on POSIX the loader, the NID bridge, the
# lv2 syscalls, guest threads, the frame clock and the RSX backend had never
# once run together.
#
# runtime/ppu/tests/smoke/ supplies what was missing: a hand-written program in
# the lifter's ABI, and tools/make_smoke_elf.py writes both the PPU ELF it is
# loaded from and the ppu_recomp.h it compiles against -- the header from
# ppu_lifter.py's own HEADER_PREAMBLE, so the synthetic title cannot drift away
# from the ABI a real one gets.
#
# The HLE NID table is generated exactly as a port generates it, so the smoke
# title's firmware imports resolve through the same bridge a game's do.
if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  Boot smoke:   no python3; ps3recomp_boot_smoke not built")
    else()
        set(SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/smoke.elf" "${SMOKE_GEN}/ppu_recomp.h"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    --out    "${SMOKE_GEN}/smoke.elf"
                    --header "${SMOKE_GEN}/ppu_recomp.h"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppu_lifter.py"
            COMMENT "Generating the smoke title's image and ppu_recomp.h")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/ppu_hle_nids.cpp"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    --out "${SMOKE_GEN}/ppu_hle_nids.cpp" --all
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nid_database.py"
            COMMENT "Generating the HLE NID -> handler table")

        add_executable(ps3recomp_boot_smoke
            runtime/ppu/tests/boot_main.cpp
            runtime/ppu/tests/smoke/smoke_recomp.c
            runtime/ppu/ppu_loader.cpp
            runtime/ppu/ppu_hle.cpp
            runtime/ppu/ppu_fs.cpp
            runtime/ppu/ppu_sysprx.cpp
            "${SMOKE_GEN}/ppu_hle_nids.cpp"
            "${SMOKE_GEN}/ppu_recomp.h"
        )
        target_include_directories(ps3recomp_boot_smoke PRIVATE
            "${SMOKE_GEN}"                                  # the generated ppu_recomp.h
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/memory"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_link_libraries(ps3recomp_boot_smoke PRIVATE ps3recomp_runtime)
        set_target_properties(ps3recomp_boot_smoke PROPERTIES LINKER_LANGUAGE CXX)

        # Where the runnable image lands, so CI and a developer both say
        #   ./build/ps3recomp_boot_smoke build/smoke/smoke.elf
        message(STATUS "  Boot smoke:   ps3recomp_boot_smoke + smoke/smoke.elf")

        # The same lift again, arranged the way a port's own tree arranges one:
        # the generated ppu_recomp.h and the lifted source in one directory,
        # which is what templates/project wants for -DRECOMP_DIR. The header is
        # generated into the build tree and the source lives in the repository,
        # so a directory holding both has to be made rather than pointed at.
        #
        # This is what lets CI build the project template against a real lift
        # and run it -- the check that a port started from templates/project
        # still boots, on a platform where it can be run.
        set(SMOKE_LIFT "${SMOKE_GEN}/lift")
        set(SMOKE_LIFT_SRC
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c")
        add_custom_command(
            OUTPUT "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SMOKE_LIFT}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT}/ppu_recomp.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_LIFT_SRC}" "${SMOKE_LIFT}/smoke_recomp.c"
            DEPENDS "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT_SRC}"
            COMMENT "Staging the smoke title's lift as a RECOMP_DIR")
        add_custom_target(ps3recomp_smoke_lift ALL
            DEPENDS "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c")
        message(STATUS "  Smoke lift:   smoke/lift (a RECOMP_DIR for templates/project)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
