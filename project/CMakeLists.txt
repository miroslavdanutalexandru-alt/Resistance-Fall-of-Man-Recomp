cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PORT_NAME)
    set(PORT_NAME ResistanceFoM)
endif()
# CMake's compiler checks build in Debug by default, which links the debug CRT (msvcrtd.lib). The portable MSVC
# libraries (Setup -PortableTools, xwin) have only the release CRT, so configure failed there.
if(NOT DEFINED CMAKE_TRY_COMPILE_CONFIGURATION)
    set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
endif()
project(${PORT_NAME}
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# Resistance port: CPU target (build_cpu.bat; README "CPU build options")
# ---------------------------------------------------------------------------
#   PORT_ARCH=AVX2      (build_cpu.bat) x86-64-v3 everywhere: the runtime and the
#                       hand-written code with -march=x86-64-v3 as well (the lifted
#                       PPU/SPU code already is: RFOM_PPU_V3/RFOM_SPU_AVX2). FMA
#                       contraction stays off (-ffp-contract=off) in all of it.
#   PORT_ARCH=mixed     (default, also "") AVX2 lifted code, x86-64 + SSE4.1 runtime and
#                       hand-written code (build.bat)
#   PORT_ARCH=baseline  no AVX anywhere (x86-64 + SSE4.1, lifted code too) for older CPUs;
#                       forces RFOM_PPU_V3=OFF and RFOM_SPU_AVX2=OFF
#                       (build_cpu.bat baseline -> build_baseline\, run.bat picks it
#                       when the launcher finds no AVX2)
set(PORT_ARCH "" CACHE STRING "CPU target: mixed (default when empty) | AVX2 (x86-64-v3) | baseline")
if(PORT_ARCH STREQUAL "")
    set(PORT_ARCH mixed)
endif()
if(NOT PORT_ARCH MATCHES "^(AVX2|mixed|baseline)$")
    message(FATAL_ERROR "PORT_ARCH must be AVX2, mixed or baseline (got '${PORT_ARCH}')")
endif()
if(PORT_ARCH STREQUAL "baseline")
    set(RFOM_PPU_V3 OFF)
    set(RFOM_SPU_AVX2 OFF)
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    port_hitchcatch.cpp   # PORT_HITCHCATCH=<ms>: key-thread stacks on frame hitches (off by default)
    rfom_mods.cpp     # RFOM_MODS: mods folder (file overrides)
    rfom_pcfeat.cpp   # F3 performance overlay, toasts
    rfom_trophies.cpp # trophy unlock toasts / Saved Games record (sceNpTrophy hook)
    rfom_cheats.cpp   # optional single-player cheats (God mode, infinite ammo / grenades)
    rfom_pcextra.cpp  # PC extras: More PC Options, photo mode, movie skip, benchmark, Discord
    rfom_menurows.cpp # R1_MENU_ROWS: Options > PC SETTINGS (pause menu) / PC rows in the game's Audio / Video options (Lua chunk swaps)
    rfom_pctext.cpp   # RFOM_PC_TEXT: console wording -> PC wording
    rfom_reflex.cpp   # RFOM_REFLEX: NVIDIA Reflex via NVAPI (tools/nvapi)
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# NVIDIA Reflex: NVAPI headers + static lib (tools/nvapi, MIT). Scoped to the
# one source so no other compile command changes. Without the SDK the file
# builds as a no-op and RFOM_REFLEX keeps the waitable swap chain.
get_filename_component(RFOM_NVAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/nvapi" ABSOLUTE)
if(WIN32 AND EXISTS "${RFOM_NVAPI_DIR}/nvapi.h" AND EXISTS "${RFOM_NVAPI_DIR}/amd64/nvapi64.lib")
    set_source_files_properties(rfom_reflex.cpp PROPERTIES INCLUDE_DIRECTORIES "${RFOM_NVAPI_DIR}")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${RFOM_NVAPI_DIR}/amd64/nvapi64.lib")
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
# Resistance port: RelWithDebInfo hands clang-cl /Ob1, which inlines ONLY
# functions spelt `inline` -- every plain static helper in the C runtime, and
# every in-class C++ method and lambda (std::atomic, lock guards...), stayed an
# out-of-line call, even under the -O2 the runtime adds (a later -O2 does not
# undo /Ob1). /Ob2 restores normal inlining for the runtime library and the
# hand-written sources. The lifted PPU and SPU TUs are left as they are: their
# helpers are already `static inline`, inlining lifted functions into each
# other would grow the host frames of deeply nested guest calls, and their
# command lines stay unchanged so they do not rebuild.
# -DRFOM_FULL_INLINE=OFF builds the old way.
option(RFOM_FULL_INLINE "Normal inlining (/Ob2) outside the lifted PPU code" ON)
if(RFOM_FULL_INLINE AND MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE /Ob2)
    set_source_files_properties(main.cpp stubs.cpp ${PS3RECOMP_SCAFFOLD}
        PROPERTIES COMPILE_OPTIONS "/Ob2")
endif()

if(SPU_SOURCES)
    # Resistance port: RFOM_SPU_FAST compiles the per-access LS watch/probe check
    # (SPU_LS_NO_DBG) and the runtime SPU_XFLOAT_EXACT switch (SPU_XF_NO_EXACT)
    # out of the lifted SPU code. Each hid an out-of-line call behind a branch
    # on every LS access / float op, so the compiler kept every register in
    # memory: jobmod_6dc227fd went from 60k to 20k instructions.
    option(RFOM_SPU_FAST "Lifted SPU code without LS debug probes / exact-float switch" ON)
    if(RFOM_SPU_FAST)
        set(_spu_defs SPU_USE_DISPATCH_MT SPU_LS_NO_DBG SPU_XF_NO_EXACT)
    else()
        set(_spu_defs SPU_USE_DISPATCH_MT)
    endif()
    # Resistance port: SPU_XF_RZ_TRUST drops the +-FLT_MAX output clamp of every
    # lifted float op (an identity under RZ, which the runtime then forces in
    # spu_thread_fp_init; 240M-op equivalence test in spu_native_1115.md).
    option(RFOM_SPU_XF_RZ_TRUST "Lifted SPU float ops: rely on RZ for saturation" ON)
    if(RFOM_SPU_FAST AND RFOM_SPU_XF_RZ_TRUST)
        list(APPEND _spu_defs SPU_XF_RZ_TRUST)
        target_compile_definitions(ps3recomp_runtime PRIVATE SPU_XF_RZ_TRUST)
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS "${_spu_defs}")
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Resistance port: the SPU helpers have SSE4.1/AVX2 paths (pshufb shufb,
        # vector float ops) that the default x86-64 baseline compiled out --
        # shufb alone was ~20% of the SPU job thread as a byte loop.
        # -DRFOM_SPU_AVX2=OFF builds the SSE4.1 baseline (the runtime needs SSE4.1 anyway).
        option(RFOM_SPU_AVX2 "Build lifted SPU code with AVX2" OFF)
        if(RFOM_SPU_AVX2 AND MSVC)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;/arch:AVX2;/GS-;/clang:-ffp-contract=off")
        elseif(RFOM_SPU_AVX2)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-mavx2;-ffp-contract=off")
        elseif(MSVC)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;/clang:-msse4.1;/clang:-ffp-contract=off")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1;-ffp-contract=off")
        endif()
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # clang-cl reads -Wall as /Wall (= -Weverything): hundreds of MB of warnings
    # in the build log and console. /W3 is its usual level.
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${PROJECT_NAME} PRIVATE /W3)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
# Resistance port: the lifted PPU code also targets x86-64-v3 (AVX2/FMA/BMI2/
# MOVBE -- any Zen/Haswell or later). Without FMA every PPC fmadd lowered to a
# CALL into the CRT's software fma() (900+ call sites in one lifted file), and
# without SSE4.1 nearbyint/trunc were calls too. Contraction stays off so only
# the lifter's explicit fma() fuses. -DRFOM_PPU_V3=OFF builds the baseline.
option(RFOM_PPU_V3 "Build lifted PPU code for x86-64-v3" ON)
# v163: the lifted PPU code takes the guest-memory bounds as constants and its
# slow paths through preserve_most wrappers (ppu_recomp.h). -DRFOM_PPU_FIXED=OFF
# builds the old runtime-bounds path.
option(RFOM_PPU_FIXED "Lifted PPU code: constant VM bounds + preserve_most slow paths" ON)
if(RFOM_PPU_FIXED)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_DEFINITIONS "PPU_FIXED_BOUNDS")
endif()
if(MSVC AND RFOM_PPU_V3)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj;/clang:-O2;/clang:-march=x86-64-v3;/clang:-ffp-contract=off;/clang:-fno-math-errno")
elseif(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj;/clang:-O2;/clang:-msse4.1;/clang:-ffp-contract=off")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()
# The lifted TUs declare `extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)` (and other TLS
# externs) without a visible initialiser, so clang-cl puts an on-demand TLS-init guard (__tls_guard +
# __dyn_tls_on_demand_init) on every access: ~11.5k per TU in ppu_recomp_001, about two per guest call.
# Every PPU TLS variable is POD and constant-initialised (__declspec(thread) cannot have dynamic init),
# so the guard never does anything. -DPORT_PPU_TLS_NOGUARD=OFF restores it. Changing it rebuilds all PPU TUs.
option(PORT_PPU_TLS_NOGUARD "Lifted PPU code: /Zc:tlsGuards- (no on-demand TLS init guards)" ON)
if(MSVC AND PORT_PPU_TLS_NOGUARD)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/Zc:tlsGuards-")
endif()

# ---------------------------------------------------------------------------
# Memory-aware build (Setup.exe, docs\INSTALL.md): one lifted ppu_recomp_*.cpp
# can take up to ~8 GB of RAM in clang-cl, so those files get their own Ninja
# job pool "ppu" and the rest of the build keeps the normal -j. Pool size:
# -DPORT_PPU_JOBS=N or the PORT_PPU_JOBS environment variable (Setup.exe sets
# it from free RAM), else max(1, (total RAM GB - 6) / 8). Per-file pools need
# CMake 4.2+; older CMake puts the whole game target in the pool (slower, same
# memory bound). -DPORT_PPU_JOB_POOL=OFF restores the old behaviour.
# ---------------------------------------------------------------------------
option(PORT_PPU_JOB_POOL "Limit parallel compiles of the lifted PPU code by RAM (Ninja job pool)" ON)
if(PORT_PPU_JOB_POOL AND CMAKE_GENERATOR MATCHES "Ninja")
    if(DEFINED PORT_PPU_JOBS AND NOT "${PORT_PPU_JOBS}" STREQUAL "")
        set(_ppu_jobs "${PORT_PPU_JOBS}")
    elseif(NOT "$ENV{PORT_PPU_JOBS}" STREQUAL "")
        set(_ppu_jobs "$ENV{PORT_PPU_JOBS}")
    else()
        cmake_host_system_information(RESULT _ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
        math(EXPR _ppu_jobs "(${_ram_mb} / 1024 - 6) / 8")
    endif()
    if(NOT _ppu_jobs MATCHES "^[0-9]+$" OR _ppu_jobs LESS 1)
        set(_ppu_jobs 1)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS ppu=${_ppu_jobs})
    set(_ppu_pool_srcs ${RECOMP_SOURCES})
    list(FILTER _ppu_pool_srcs INCLUDE REGEX "ppu_recomp_[0-9]+\\.cpp$")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.2)
        set_property(SOURCE ${_ppu_pool_srcs} PROPERTY JOB_POOL_COMPILE ppu)
        message(STATUS "PPU job pool:    ${_ppu_jobs} parallel lifted-PPU compile(s)")
    else()
        set_property(TARGET ${PROJECT_NAME} PROPERTY JOB_POOL_COMPILE ppu)
        message(STATUS "PPU job pool:    ${_ppu_jobs} (CMake < 4.2: whole game target)")
    endif()
endif()
# ---------------------------------------------------------------------------
# Resistance port: CPU build options (clang-cl only; docs in the R3 tree,
# scratch\cpu_opt.md). Defaults change no floating-point semantics.
#   PORT_C_TLS_NOGUARD=ON  (default) drop clang-cl's per-access TLS init guard
#                          in C sources: C has no dynamically initialised
#                          thread-locals and every thread of an exe runs the
#                          TLS callback first, so the guard is dead code.
#   PORT_LTO=ON            ThinLTO for runtime, hand-written and SPU code
#                          (PORT_LTO_PPU=ON adds the lifted PPU code).
#   PORT_PGO=gen|use       clang instrumentation PGO, same scope; gen writes
#                          pgo\game-%p.profraw, use takes PORT_PGO_PROFILE=<.profdata>.
# ---------------------------------------------------------------------------
option(PORT_C_TLS_NOGUARD "Drop clang-cl's TLS init guards in C sources" ON)
option(PORT_LTO "ThinLTO for runtime, hand-written and lifted SPU code" OFF)
option(PORT_LTO_PPU "ThinLTO for the lifted PPU code too" OFF)
set(PORT_PGO "off" CACHE STRING "Profile-guided optimisation: off | gen | use")
set(PORT_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for PORT_PGO=use")
option(PORT_PGO_PPU "PGO for the lifted PPU code too" OFF)
# Short spellings (scratch/lto_pgo.md), all default OFF/empty = the default build:
#   PORT_THINLTO=ON          = PORT_LTO=ON + PORT_LTO_PPU=ON (runtime and all lifted code),
#                              plus /Gw. PORT_THINLTO_SCOPE=nopu leaves the lifted PPU code
#                              out; =hot puts only the PPU files named in PORT_LTO_PPU_ONLY in.
#   PORT_LTO_JOBS=N          lld-link ThinLTO backend threads (default: all without the PPU
#                              code, else the RAM-based PPU job pool size: each backend of a
#                              lifted PPU module needs as much RAM as its compile).
#   PORT_LTO_CACHE=OFF       no /lldltocache (default ON: <build>/lto_cache).
#   PORT_LINK_ICF=ON         /OPT:SAFEICF as well as /OPT:REF.
#   PORT_PGO_GEN=ON          = PORT_PGO=gen;  PORT_PGO_USE=<.profdata> = PORT_PGO=use.
option(PORT_THINLTO "ThinLTO for the runtime and the lifted code (PORT_THINLTO_SCOPE)" OFF)
set(PORT_THINLTO_SCOPE "all" CACHE STRING "PORT_THINLTO scope: all | nopu | hot")
set(PORT_LTO_PPU_ONLY "" CACHE STRING "PPU LTO only for these ppu_recomp_NNN names (PORT_THINLTO_SCOPE=hot)")
set(PORT_LTO_JOBS "" CACHE STRING "lld-link ThinLTO backend threads (empty: automatic)")
option(PORT_LTO_CACHE "ThinLTO incremental cache (/lldltocache)" ON)
option(PORT_LINK_ICF "Link with /OPT:SAFEICF too" OFF)
option(PORT_PGO_GEN "Instrumented PGO build (= PORT_PGO=gen)" OFF)
set(PORT_PGO_USE "" CACHE FILEPATH "Merged .profdata: PGO-optimised build (= PORT_PGO=use)")
if(PORT_THINLTO)
    set(PORT_LTO ON)
    if(PORT_THINLTO_SCOPE STREQUAL "all" OR PORT_THINLTO_SCOPE STREQUAL "hot")
        set(PORT_LTO_PPU ON)
    elseif(NOT PORT_THINLTO_SCOPE STREQUAL "nopu")
        message(FATAL_ERROR "PORT_THINLTO_SCOPE must be all, nopu or hot (got '${PORT_THINLTO_SCOPE}')")
    endif()
endif()
if(PORT_PGO_GEN AND PORT_PGO_USE)
    message(FATAL_ERROR "PORT_PGO_GEN and PORT_PGO_USE are exclusive")
elseif(PORT_PGO_GEN)
    set(PORT_PGO gen)
elseif(PORT_PGO_USE)
    set(PORT_PGO use)
    set(PORT_PGO_PROFILE "${PORT_PGO_USE}")
endif()
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Hand-written sources of the exe = everything but the lifted PPU/SPU code.
    get_target_property(_all_srcs ${PROJECT_NAME} SOURCES)
    set(_hand_srcs "")
    foreach(s IN LISTS _all_srcs)
        if(s MATCHES "\\.(c|cc|cpp|cxx)$")
            list(FIND RECOMP_SOURCES "${s}" _i)
            list(FIND SPU_SOURCES "${s}" _j)
            if(_i EQUAL -1 AND _j EQUAL -1)
                list(APPEND _hand_srcs "${s}")
            endif()
        endif()
    endforeach()
    # RFOM_FULL_INLINE also covers the hand-written sources not named above
    # (rfom_*.cpp, the HLE NID table); they were still /Ob1.
    if(RFOM_FULL_INLINE)
        foreach(s IN LISTS _hand_srcs)
            get_source_file_property(_o "${s}" COMPILE_OPTIONS)
            if(NOT _o MATCHES "/Ob2")
                set_property(SOURCE "${s}" APPEND PROPERTY COMPILE_OPTIONS "/Ob2")
            endif()
        endforeach()
    endif()
    # /debug makes lld-link default to /OPT:NOREF; REF drops unreferenced
    # COMDATs only (298 -> 283 MB on R3), relink time unchanged. Nothing in
    # the port locates code by section or by symbol name. ICF is left off.
    option(PORT_LINK_OPT_REF "Link with /OPT:REF (drop unreferenced functions/data)" ON)
    if(PORT_LINK_OPT_REF)
        target_link_options(${PROJECT_NAME} PRIVATE /OPT:REF)
    endif()
    if(PORT_C_TLS_NOGUARD)
        target_compile_options(ps3recomp_runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zc:tlsGuards->)
        target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zc:tlsGuards->)
    endif()
    # /GS- drops the stack-cookie checks (2300+ sites in the runtime, the D3D12 renderer included);
    # /Gw puts globals in their own COMDATs for /OPT:REF. No floating-point or behaviour change.
    option(PORT_GS_OFF "/GS- and /Gw for the runtime and the hand-written code" ON)
    if(PORT_GS_OFF)
        target_compile_options(ps3recomp_runtime PRIVATE /GS- /Gw)
        set_property(SOURCE ${_hand_srcs} APPEND PROPERTY COMPILE_OPTIONS "/GS-;/Gw")
    endif()
    if(PORT_ARCH STREQUAL "AVX2")
        # x86-64-v3 = what the lifted PPU code and port_cpucheck.h use. Contraction stays
        # off: the runtime's float paths were validated without FMA, and a fused a*b+c
        # rounds differently from the PS3's separate multiply and add.
        target_compile_options(ps3recomp_runtime PRIVATE /clang:-march=x86-64-v3 /clang:-ffp-contract=off)
        set_property(SOURCE ${_hand_srcs} APPEND PROPERTY COMPILE_OPTIONS "/clang:-march=x86-64-v3;/clang:-ffp-contract=off")
        message(STATUS "CPU target:      AVX2 / x86-64-v3 (runtime + hand-written + lifted)")
    elseif(PORT_ARCH STREQUAL "mixed")
        message(STATUS "CPU target:      mixed (AVX2 lifted code, x86-64 + SSE4.1 runtime and hand-written code)")
    elseif(PORT_ARCH STREQUAL "baseline")
        message(STATUS "CPU target:      baseline x86-64 + SSE4.1 (no AVX; RFOM_PPU_V3/RFOM_SPU_AVX2 off)")
    endif()
    set(_ppu_srcs ${RECOMP_SOURCES})
    list(FILTER _ppu_srcs INCLUDE REGEX "ppu_recomp_[0-9]+\\.cpp$")
    set(_opt_srcs ${_hand_srcs} ${SPU_SOURCES})
    if(PORT_LTO)
        set(_lto_srcs ${_opt_srcs})
        # The other generated units (HLE NID table, function table) are plain objects otherwise; lld-link
        # reports their strong ppu_hle_register_all as a duplicate of ppu_hle.cpp's weak bitcode one.
        set(_recomp_misc ${RECOMP_SOURCES})
        list(FILTER _recomp_misc EXCLUDE REGEX "ppu_recomp_[0-9]+\\.cpp$")
        list(APPEND _lto_srcs ${_recomp_misc})
        set(_lto_ppu "")
        if(PORT_LTO_PPU)
            set(_lto_ppu ${_ppu_srcs})
            if(PORT_THINLTO AND PORT_THINLTO_SCOPE STREQUAL "hot")
                set(_lto_ppu "")
                foreach(s IN LISTS _ppu_srcs)
                    get_filename_component(_n "${s}" NAME_WE)
                    if(_n IN_LIST PORT_LTO_PPU_ONLY)
                        list(APPEND _lto_ppu "${s}")
                    endif()
                endforeach()
            endif()
            list(APPEND _lto_srcs ${_lto_ppu})
        endif()
        list(LENGTH _lto_ppu _lto_nppu)
        target_compile_options(ps3recomp_runtime PRIVATE -flto=thin)
        set_property(SOURCE ${_lto_srcs} APPEND PROPERTY COMPILE_OPTIONS "-flto=thin")
        if(PORT_THINLTO)
            # Per-variable COMDATs so /OPT:REF can drop dead data in the non-LTO objects too.
            target_compile_options(${PROJECT_NAME} PRIVATE /Gw)
            target_compile_options(ps3recomp_runtime PRIVATE /Gw)
        endif()
        # lld-link runs the ThinLTO backends; a backend of a lifted PPU module
        # needs about as much RAM as compiling it, so with PPU modules in LTO
        # the thread count defaults to the RAM-based PPU job pool size.
        if(NOT "${PORT_LTO_JOBS}" STREQUAL "")
            set(_lto_jobs "${PORT_LTO_JOBS}")
        elseif(_lto_nppu GREATER 0)
            cmake_host_system_information(RESULT _ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
            math(EXPR _lto_jobs "(${_ram_mb} / 1024 - 6) / 8")
            if(_lto_jobs LESS 1)
                set(_lto_jobs 1)
            endif()
        else()
            set(_lto_jobs all)
        endif()
        target_link_options(${PROJECT_NAME} PRIVATE /opt:lldltojobs=${_lto_jobs} /INCREMENTAL:NO)
        # The cache lets a relink after a small change re-optimise only the modules that changed.
        if(PORT_LTO_CACHE)
            target_link_options(${PROJECT_NAME} PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto_cache")
        endif()
        message(STATUS "LTO:             ThinLTO (runtime, hand-written, SPU; lifted PPU files: ${_lto_nppu}; backend jobs: ${_lto_jobs})")
    endif()
    if(PORT_LINK_ICF)
        target_link_options(${PROJECT_NAME} PRIVATE /OPT:SAFEICF)
    endif()
    if(PORT_PGO STREQUAL "gen" OR PORT_PGO STREQUAL "use")
        set(_pgo_srcs ${_opt_srcs})
        if(PORT_PGO_PPU)
            list(APPEND _pgo_srcs ${_ppu_srcs})
        endif()
        if(PORT_PGO STREQUAL "gen")
            set(_pgo_flag "-fprofile-instr-generate")
            get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            file(GLOB _prof_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
            if(NOT _prof_rt)
                message(FATAL_ERROR "PORT_PGO=gen: clang_rt.profile-x86_64.lib not found under ${_llvm_bin}/../lib/clang")
            endif()
            list(GET _prof_rt 0 _prof_rt)
            target_link_libraries(${PROJECT_NAME} PRIVATE "${_prof_rt}")
            # pgo_gen.cpp flushes the counters every PORT_PGO_DUMP_S s and at exit.
            target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pgo_gen.cpp")
            message(STATUS "PGO:             instrumented (writes LLVM_PROFILE_FILE)")
        else()
            if(NOT EXISTS "${PORT_PGO_PROFILE}")
                message(FATAL_ERROR "PORT_PGO=use needs PORT_PGO_PROFILE=<merged .profdata> (got '${PORT_PGO_PROFILE}')")
            endif()
            file(TO_NATIVE_PATH "${PORT_PGO_PROFILE}" _pd)
            set(_pgo_flag "-fprofile-instr-use=${_pd};-Wno-profile-instr-unprofiled;-Wno-profile-instr-out-of-date;-Wno-backend-plugin")
            message(STATUS "PGO:             optimising with ${PORT_PGO_PROFILE}")
        endif()
        target_compile_options(ps3recomp_runtime PRIVATE ${_pgo_flag})
        set_property(SOURCE ${_pgo_srcs} APPEND PROPERTY COMPILE_OPTIONS "${_pgo_flag}")
        if(PORT_PGO STREQUAL "use")
            # The profile is an input of every PGO object: a re-merged .profdata must recompile them
            # (Ninja does not track it otherwise). The runtime's sources live in another directory scope.
            set_property(SOURCE ${_pgo_srcs} APPEND PROPERTY OBJECT_DEPENDS "${PORT_PGO_PROFILE}")
            get_target_property(_rt_srcs ps3recomp_runtime SOURCES)
            get_target_property(_rt_dir ps3recomp_runtime SOURCE_DIR)
            set(_rt_abs "")
            foreach(s IN LISTS _rt_srcs)
                if(s MATCHES "\\.(c|cc|cpp|cxx)$" AND NOT s MATCHES "^\\$<")
                    if(NOT IS_ABSOLUTE "${s}")
                        set(s "${_rt_dir}/${s}")
                    endif()
                    list(APPEND _rt_abs "${s}")
                endif()
            endforeach()
            set_property(SOURCE ${_rt_abs} TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY OBJECT_DEPENDS "${PORT_PGO_PROFILE}")
        endif()
    endif()
endif()

# Resistance port: an AVX2 build (anything but PORT_ARCH=baseline) needs x86-64-v3;
# port_cpucheck.h (included by main.cpp) then refuses older CPUs with a clear message
# instead of an illegal-instruction crash.
option(PORT_CPU_CHECK "Startup check for the CPU features the build needs: AVX2, or SSE4.1 for PORT_ARCH=baseline (port_cpucheck.h)" ON)
if(PORT_CPU_CHECK AND (RFOM_PPU_V3 OR RFOM_SPU_AVX2 OR PORT_ARCH STREQUAL "AVX2"))
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_REQUIRE_X86_64_V3=1)
elseif(PORT_CPU_CHECK)
    # baseline: the runtime and the lifted code are built with -msse4.1
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_REQUIRE_SSE41=1)
endif()
# PORT_OPT_EXTRA=ON (default OFF): /Gw (per-variable COMDATs so /OPT:REF can
# drop dead data) for everything, /GS- (no stack cookies) for the lifted PPU
# code. Only ~44 of the functions in a lifted PPU TU carry a cookie and the
# runtime is already /O2 (= /Oi /Ot /Gy) + /Ob2, so the expected gain is small;
# no floating-point semantics change. Build time unchanged. ThinLTO is the
# separate PORT_LTO switch (much longer links, see above).
option(PORT_OPT_EXTRA "Extra codegen flags: /Gw everywhere, /GS- for lifted PPU code" OFF)
if(PORT_OPT_EXTRA AND MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /Gw)
    target_compile_options(ps3recomp_runtime PRIVATE /Gw)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/GS-")
    message(STATUS "Extra opt:       /Gw, lifted PPU /GS-")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
