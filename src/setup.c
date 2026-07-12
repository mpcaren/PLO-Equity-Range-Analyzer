/* setup.c — plo5setup: one-time per-machine setup and calibration.
 *
 * Run this once after building (or after copying the tools to a new
 * machine). It:
 *   1. detects the CPU (name + logical cores),
 *   2. benchmarks the equity engine on this machine,
 *   3. builds the preflop rank table (plo5rank.bin) if it is missing —
 *      the one slow step, a few minutes, never needed again,
 *   4. writes plo5config.ini next to the executables with the thread
 *      count and Monte Carlo trial presets calibrated to this CPU.
 *
 * Everything still works without running it (the tools auto-detect
 * cores and use stock defaults) — this just fits the defaults to the
 * machine. Re-run any time; delete plo5rank.bin first to rebuild it.
 *
 * Build (MSVC): cl /O2 /std:c11 /utf-8 /Isrc src\setup.c src\plo5.c /Fe:plo5setup.exe
 * Build (gcc) : gcc -O3 -std=c11 -Isrc -o plo5setup src/setup.c src/plo5.c -lm -pthread
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plo5.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define HAVE_CPUID 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define HAVE_CPUID 2
#endif

static double now_s(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static int ncores(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* CPU brand string via CPUID (x86/x64); "unknown CPU" elsewhere */
static void cpu_name(char *out, int cap)
{
    snprintf(out, (size_t)cap, "unknown CPU");
#ifdef HAVE_CPUID
    unsigned r[12] = {0};
#if HAVE_CPUID == 1
    int t[4];
    __cpuid(t, 0x80000000);
    if ((unsigned)t[0] < 0x80000004u) return;
    for (int i = 0; i < 3; i++) {
        __cpuid(t, 0x80000002u + (unsigned)i);
        memcpy(r + i * 4, t, 16);
    }
#else
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u) return;
    for (unsigned i = 0; i < 3; i++) {
        __get_cpuid(0x80000002u + i, r + i * 4, r + i * 4 + 1,
                    r + i * 4 + 2, r + i * 4 + 3);
    }
#endif
    char s[49];
    memcpy(s, r, 48);
    s[48] = 0;
    char *p = s;
    while (*p == ' ') p++;
    if (*p) snprintf(out, (size_t)cap, "%s", p);
#endif
}

/* directory of this executable ("" -> use cwd) */
static void exe_dir(char *out, int cap)
{
    out[0] = 0;
#ifdef _WIN32
    char exe[1024];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            snprintf(out, (size_t)cap, "%s\\", exe);
        }
    }
#endif
}

/* round to two significant digits for friendly preset numbers */
static uint64_t nice_round(double v)
{
    if (v < 10) return (uint64_t)(v + 0.5);
    double m = 1;
    while (v >= 100) { v /= 10; m *= 10; }
    return (uint64_t)(floor(v + 0.5) * m);
}

static uint64_t clampu(uint64_t v, uint64_t lo, uint64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void progress(int done, int total, void *ud)
{
    (void)ud;
    static int last = -1;
    int pct = (int)(100.0 * done / (total > 0 ? total : 1));
    if (pct != last) {
        printf("\r  ranking all starting hands... %d%%", pct);
        fflush(stdout);
        last = pct;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("plo5setup — one-time machine setup for the PLO5 tools\n\n");

    plo5_init();

    /* 1. detect */
    char cpu[64];
    cpu_name(cpu, sizeof cpu);
    int nt = ncores();
    printf("CPU      %s\n", cpu);
    printf("threads  %d\n\n", nt);

    /* 2. benchmark: preflop MC, one fixed hand vs one random */
    printf("benchmarking the equity engine");
    fflush(stdout);
    int hands[10] = { 0 };
    const char *hero[5] = { "As", "Ah", "Ks", "Kd", "7c" };
    for (int i = 0; i < 5; i++) hands[i] = plo5_parse_card(hero[i]);
    for (int i = 5; i < 10; i++) hands[i] = PLO5_RANDOM;

    plo5_result r;
    uint64_t bench = 1000000;
    double el = 0;
    for (int pass = 0; pass < 6; pass++) {          /* grow until >= 0.7 s */
        double t0 = now_s();
        int rc = plo5_equity(hands, 2, NULL, 0, NULL, 0, bench, 0,
                             12345, nt, &r);
        el = now_s() - t0;
        if (rc != PLO5_OK) {
            fprintf(stderr, "\nerror: engine benchmark failed (code %d)\n", rc);
            return 1;
        }
        printf(".");
        fflush(stdout);
        if (el >= 0.7) break;
        bench *= 4;
    }
    double tps = (double)bench / (el > 1e-9 ? el : 1e-9);
    printf(" %.1fM trials/sec\n\n", tps / 1e6);

    /* 3. preflop rank table (skipped when already present) */
    char dir[1024], rankpath[1200], inipath[1200];
    exe_dir(dir, sizeof dir);
    snprintf(rankpath, sizeof rankpath, "%splo5rank.bin", dir);
    snprintf(inipath, sizeof inipath, "%splo5config.ini", dir);

    if (plo5_ranks_load(rankpath) == PLO5_OK) {
        printf("preflop rank table: found (%s)\n", rankpath);
        printf("  delete the file and re-run plo5setup to rebuild it\n\n");
    } else {
        printf("preflop rank table: missing — building it now (one time,\n"
               "  a few minutes; ranks all 2,598,960 starting hands)\n");
        int rc = plo5_ranks_generate(rankpath, 25000, nt, 20260704,
                                     progress, NULL);
        printf("\n");
        if (rc != PLO5_OK) {
            fprintf(stderr, "error: rank table build failed (code %d)\n", rc);
            return 1;
        }
        printf("  written to %s\n\n", rankpath);
    }

    /* 4. calibrate UI presets: Fast ~0.3s, Balanced ~1.5s, Precise ~12s
     * of preflop-speed work, clamped to sane ranges */
    uint64_t fast = clampu(nice_round(tps * 0.3), 100000, 1000000);
    uint64_t bal  = clampu(nice_round(tps * 1.5), 500000, 8000000);
    uint64_t prec = clampu(nice_round(tps * 12.0), 2000000, 50000000);

    FILE *f = fopen(inipath, "w");
    if (!f) {
        fprintf(stderr, "error: cannot write %s\n", inipath);
        return 1;
    }
    fprintf(f,
        "# plo5config.ini - machine-specific settings, written by plo5setup.\n"
        "# Safe to edit or delete (tools auto-detect sensible defaults).\n"
        "# Detected: %s, %d threads, ~%.1fM preflop trials/sec\n"
        "\n"
        "threads=%d\n"
        "\n"
        "# web UI precision presets, calibrated to this CPU\n"
        "trials_fast=%llu\n"
        "trials_balanced=%llu\n"
        "trials_precise=%llu\n"
        "\n"
        "# server hosting (plo5web) - uncomment to change the defaults:\n"
        "# port=8722\n"
        "# lan=1\n"
        "# password=pick-something\n",
        cpu, nt, tps / 1e6, nt,
        (unsigned long long)fast, (unsigned long long)bal,
        (unsigned long long)prec);
    fclose(f);

    printf("config written: %s\n", inipath);
    printf("  threads=%d   presets: Fast %lluk / Balanced %.1fM / Precise %.0fM\n\n",
           nt, (unsigned long long)(fast / 1000), bal / 1e6, prec / 1e6);
    printf("done — you only need to run this once on a machine.\n"
           "next: plo5web        (local browser UI)\n"
           "      plo5web --lan  (host for other devices, password-protected)\n");
    return 0;
}
