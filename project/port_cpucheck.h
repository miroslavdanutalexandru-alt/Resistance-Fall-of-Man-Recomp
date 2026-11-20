/* Resistance PC ports: refuse to start on a CPU the build cannot run on.
 *
 * The lifted PPU code is built for x86-64-v3 (-march=x86-64-v3: AVX2, FMA,
 * BMI1/2, LZCNT, MOVBE, F16C), the lifted SPU code with /arch:AVX2 and, with
 * PORT_ARCH=AVX2 (build_cpu.bat avx2), the runtime and the hand-written code too. On an
 * older CPU the first such instruction would die with an illegal-instruction
 * crash; this check runs from a CRT initializer section (.CRT$XCB, before the
 * ordinary C++ static constructors of any TU) and shows a clear message
 * instead. Its own two functions are compiled without SSE3 and everything above
 * it (SSSE3/SSE4.x/POPCNT/AVX/FMA/BMI/LZCNT/MOVBE/F16C), so the check itself runs
 * on any x86-64 CPU. Active when the build defines PORT_REQUIRE_X86_64_V3 (CMake:
 * every build with AVX2 lifted code, i.e. all but PORT_ARCH=baseline) or
 * PORT_REQUIRE_SSE41 (CMake: PORT_ARCH=baseline, whose runtime and lifted code
 * are built with -msse4.1: SSE3 + SSSE3 + SSE4.1, Intel Core 2 Penryn / AMD
 * Bulldozer or newer). Both come from PORT_CPU_CHECK (default ON).
 * PORT_SKIP_CPU_CHECK=1 skips it (virtual machines that hide CPUID bits).
 * Self-contained: include once in the file that has main(). */
#pragma once
#if defined(_WIN32) && (defined(PORT_REQUIRE_X86_64_V3) || defined(PORT_REQUIRE_SSE41)) && (defined(_M_X64) || defined(__x86_64__))
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* getenv */
#endif
namespace port_cpucheck {

#if defined(__clang__)
#define PORT_CPUCHECK_V1 __attribute__((target("no-sse3,no-avx,no-bmi,no-bmi2,no-lzcnt,no-movbe,no-f16c,no-fma,no-popcnt")))
#else
#define PORT_CPUCHECK_V1
#endif

PORT_CPUCHECK_V1 static int missing(char* b, size_t n, int* base_missing)
{
    int r[4];
    __cpuid(r, 0);
    const int maxl = r[0];
    __cpuid(r, 1);
    const unsigned c1 = (unsigned)r[2];
    unsigned b7 = 0;
    if (maxl >= 7) { __cpuidex(r, 7, 0); b7 = (unsigned)r[1]; }
    __cpuid(r, 0x80000000);
    unsigned c81 = 0;
    if ((unsigned)r[0] >= 0x80000001u) { __cpuid(r, 0x80000001); c81 = (unsigned)r[2]; }
    /* AVX state enabled by the OS (XCR0 bits 1 and 2) */
    int os_avx = 0;
    if ((c1 >> 27) & 1) os_avx = (_xgetbv(0) & 6) == 6;
    (void)b7; (void)c81; (void)os_avx;
    struct { int ok; const char* name; } f[] = {
        { (int)(c1 & 1), "SSE3" },
        { (int)((c1 >> 9) & 1), "SSSE3" },
        { (int)((c1 >> 19) & 1), "SSE4.1" },
#ifdef PORT_REQUIRE_X86_64_V3
        { (int)((c1 >> 28) & 1) && os_avx, "AVX" },
        { (int)((b7 >> 5) & 1) && os_avx, "AVX2" },
        { (int)((c1 >> 12) & 1), "FMA3" },
        { (int)((b7 >> 3) & 1), "BMI1" },
        { (int)((b7 >> 8) & 1), "BMI2" },
        { (int)((c81 >> 5) & 1), "LZCNT" },
        { (int)((c1 >> 22) & 1), "MOVBE" },
        { (int)((c1 >> 29) & 1), "F16C" },
#endif
    };
    /* f[0..2] (SSE3/SSSE3/SSE4.1) are the minimum of every build, the baseline one too */
    int k = 0, i = 0; b[0] = 0; *base_missing = 0;
    for (auto& e : f) {
        if (!e.ok) {
            if (i < 3) *base_missing = 1;
            const size_t l = strlen(b);
            snprintf(b + l, n - l, "%s%s", k++ ? ", " : "", e.name);
        }
        i++;
    }
    return k;
}

PORT_CPUCHECK_V1 static void __cdecl check()
{
    const char* s = getenv("PORT_SKIP_CPU_CHECK");
    if (s && *s == '1') return;
    char miss[128];
    int base_missing = 0;
    if (!missing(miss, sizeof miss, &base_missing)) return;
    char msg[768];
#ifdef PORT_REQUIRE_X86_64_V3
    /* below SSE4.1 the baseline build cannot run either, so it is only suggested when just the AVX2 set is missing */
    if (!base_missing)
    snprintf(msg, sizeof msg,
             "This build needs a CPU with AVX2 (x86-64-v3: Intel Haswell / 4th-gen Core or newer, "
             "AMD Ryzen / Excavator or newer).\n\nMissing on this CPU: %s\n\n"
             "This build cannot run on this processor. Build the variant without AVX2 with "
             "build_cpu.bat baseline; the launcher then starts it automatically. (PORT_SKIP_CPU_CHECK=1 skips this check, "
             "e.g. in a virtual machine that hides CPU features.)", miss);
    else
#endif
    snprintf(msg, sizeof msg,
             "This processor is below the minimum of these ports (SSE4.1: Intel Core 2 Penryn / 45 nm or newer, "
             "AMD Bulldozer / FX or newer); no build of the game can run on it.\n\nMissing on this CPU: %s\n\n"
             "(PORT_SKIP_CPU_CHECK=1 skips this check, e.g. in a virtual machine that hides CPU features.)", miss);
    fprintf(stderr, "[cpu] FATAL: unsupported CPU, missing %s\n", miss);
    fflush(stderr);
    MessageBoxA(NULL, msg, "Unsupported CPU", MB_OK | MB_ICONERROR);
    ExitProcess(3);
}

/* before the C++ dynamic initializers (.CRT$XCU) */
#pragma section(".CRT$XCB", read)
__declspec(allocate(".CRT$XCB")) static void (__cdecl* const s_check)() = check;
#if defined(__clang__)
static const void* const s_keep __attribute__((used)) = (const void*)&s_check;
#endif

} // namespace port_cpucheck
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
