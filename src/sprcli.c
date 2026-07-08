/* sprcli.c — plo5spr: stack-off thresholds across a range of SPRs.
 *
 * For each SPR: stack = SPR*pot, MDF = 1/(1+SPR), villain range trimmed
 * to its top MDF fraction (board-conditional percentiles), hero equity
 * vs the trimmed range(s), the raw no-fold-equity threshold
 * SPR/(1+nway*SPR), and the breakeven fold% when below it.
 *
 * Build (MSVC): cl /O2 /std:c11 /utf-8 /Isrc src\sprcli.c src\spr.c src\plo5.c /Fe:plo5spr.exe
 * Build (gcc) : gcc -O3 -std=c11 -Isrc -o plo5spr src/sprcli.c src/spr.c src/plo5.c -lm -pthread
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plo5.h"
#include "spr.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void usage(void)
{
    fprintf(stderr,
"plo5spr — PLO5 stack-off thresholds across SPRs\n"
"\n"
"usage: plo5spr HAND [options]\n"
"  HAND             hero's 5 cards, e.g. AhAdKhKd7s\n"
"  --board CARDS    3, 4 or 5 board cards (omit for preflop; needs\n"
"                   plo5rank.bin next to the exe)\n"
"  --pot X          current pot size (default 10)\n"
"  --opp N          villains stacking off, 1-5 (default 1)\n"
"  --spr LO:HI:STEP SPR grid (default 0.25:5:0.25), or a comma list\n"
"                   like 3,4,5,7,10\n"
"  --range LO-HI    villain starting percentile band before MDF\n"
"                   trimming (default 0-100)\n"
"  --rangefile F    villain starting range from a file: either one\n"
"                   band like \"35-100\", or one 5-card hand per line\n"
"                   (scored on the board, MDF-trimmed per SPR)\n"
"  --trials N       MC trials per row (default 200k; k/m suffix ok)\n"
"  --threads N      worker threads, 0 = all cores (default 0)\n"
"  --seed N         PRNG seed (default 42)\n"
"\n"
"example (6-way flop stack-off, SPR 3..10):\n"
"  plo5spr AhKhQsJd9d --board Ks7h2d --opp 5 --pot 12 --spr 3:10:1\n");
}

static int parse_cards(const char *s, int *out, int maxn)
{
    int n = 0, bi = 0;
    char buf[2];
    for (; *s; s++) {
        if (isspace((unsigned char)*s) || *s == ',') continue;
        buf[bi++] = *s;
        if (bi == 2) {
            if (n >= maxn) return -1;
            int c = plo5_parse_card(buf);
            if (c < 0) return -1;
            out[n++] = c;
            bi = 0;
        }
    }
    return bi == 0 ? n : -1;
}

static uint64_t parse_count(const char *s)
{
    char *e = NULL;
    double v = strtod(s, &e);
    if (e && (*e == 'k' || *e == 'K')) v *= 1e3;
    else if (e && (*e == 'm' || *e == 'M')) v *= 1e6;
    return (uint64_t)v;
}

/* "3:10:1" or "0.5,1,2,3" -> spr list; returns count or -1 */
static int parse_sprs(const char *s, double *out, int maxn)
{
    if (strchr(s, ':')) {
        double lo, hi, step;
        if (sscanf(s, "%lf:%lf:%lf", &lo, &hi, &step) != 3 ||
            step <= 0 || hi < lo)
            return -1;
        int n = 0;
        for (double v = lo; v <= hi + 1e-9 && n < maxn; v += step)
            out[n++] = v;
        return n;
    }
    int n = 0;
    const char *p = s;
    while (*p && n < maxn) {
        char *e = NULL;
        double v = strtod(p, &e);
        if (e == p) return -1;
        out[n++] = v;
        p = e;
        while (*p == ',' || *p == ' ') p++;
    }
    return n > 0 ? n : -1;
}

static const char *err_str(int rc)
{
    switch (rc) {
    case PLO5_ERR_PLAYERS: return "opponents must be 1..5";
    case PLO5_ERR_BOARD:   return "board must have 0, 3, 4 or 5 cards";
    case PLO5_ERR_CARD:    return "invalid card";
    case PLO5_ERR_DUP:     return "duplicate card";
    case PLO5_ERR_RANKS:   return "rank table missing - preflop mode needs "
                                  "plo5rank.bin (plo5calc --gen-ranks)";
    case PLO5_ERR_RANGE:   return "range cannot be dealt";
    default:               return "invalid arguments";
    }
}

static void print_row(const plo5_spr_row *r, double pot)
{
    printf("%5.2f %9.1f %9.1f  %5.1f%%  %5.1f-%-3.0f%s  %6.2f%%  %6.2f%% ±%.2f  %-3s",
           r->spr, r->stack, r->pot_final, r->mdf * 100.0,
           r->trim_lo, r->trim_hi, r->band_widened ? "*" : " ",
           r->eq_needed * 100.0,
           r->hero_eq * 100.0, r->ci95 * 100.0,
           r->profitable_no_fold ? "yes" : "no");
    if (r->profitable_no_fold)
        printf("        —\n");
    else
        printf("   %5.1f%%\n", r->breakeven_fold * 100.0);
    (void)pot;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    plo5_init();

    int hero[5];
    if (parse_cards(argv[1], hero, 5) != 5) {
        fprintf(stderr, "error: hero hand needs exactly 5 cards\n");
        return 1;
    }

    int board[5], nboard = 0;
    double pot = 10, vlo = 0, vhi = 100;
    int nopp = 1, nthreads = 0;
    uint64_t trials = 200000, seed = 42;
    double sprs[512];
    int nspr = parse_sprs("0.25:5:0.25", sprs, 512);
    const char *rangefile = NULL;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--board") == 0 && i + 1 < argc) {
            nboard = parse_cards(argv[++i], board, 5);
            if (nboard < 3) { fprintf(stderr, "error: bad --board\n"); return 1; }
        } else if (strcmp(a, "--pot") == 0 && i + 1 < argc) {
            pot = atof(argv[++i]);
        } else if (strcmp(a, "--opp") == 0 && i + 1 < argc) {
            nopp = atoi(argv[++i]);
        } else if (strcmp(a, "--spr") == 0 && i + 1 < argc) {
            nspr = parse_sprs(argv[++i], sprs, 512);
            if (nspr < 0) { fprintf(stderr, "error: bad --spr\n"); return 1; }
        } else if (strcmp(a, "--range") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf-%lf", &vlo, &vhi) != 2) {
                fprintf(stderr, "error: bad --range (want LO-HI)\n");
                return 1;
            }
        } else if (strcmp(a, "--rangefile") == 0 && i + 1 < argc) {
            rangefile = argv[++i];
        } else if (strcmp(a, "--trials") == 0 && i + 1 < argc) {
            trials = parse_count(argv[++i]);
        } else if (strcmp(a, "--threads") == 0 && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
        } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "error: unknown option %s\n", a);
            usage();
            return 1;
        }
    }
    if (pot <= 0) { fprintf(stderr, "error: pot must be > 0\n"); return 1; }
    if (nopp < 1 || nopp > PLO5_MAX_PLAYERS - 1) {
        fprintf(stderr, "error: --opp must be 1..%d\n", PLO5_MAX_PLAYERS - 1);
        return 1;
    }
    if (nthreads <= 0) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        nthreads = (int)si.dwNumberOfProcessors;
#else
        nthreads = 4;
#endif
        if (nthreads < 1) nthreads = 1;
    }

    /* villain combo list from a file (one hand per line) is scored on
     * the board and trimmed per SPR — the swap-in path for smarter
     * range construction. A file holding just "LO-HI" sets the band. */
    int (*combos)[5] = NULL;
    int ncombos = 0;
    if (rangefile) {
        FILE *f = fopen(rangefile, "r");
        if (!f) { fprintf(stderr, "error: cannot open %s\n", rangefile); return 1; }
        char line[128];
        int cap = 0;
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            if (!*p || *p == '#') continue;
            double a, b;
            if (ncombos == 0 && sscanf(p, "%lf-%lf", &a, &b) == 2) {
                vlo = a; vhi = b;
                continue;
            }
            int h[5];
            if (parse_cards(p, h, 5) != 5) {
                fprintf(stderr, "error: bad hand in %s: %s", rangefile, p);
                fclose(f);
                return 1;
            }
            if (ncombos >= cap) {
                cap = cap ? cap * 2 : 256;
                combos = realloc(combos, (size_t)cap * sizeof *combos);
            }
            memcpy(combos[ncombos++], h, sizeof h);
        }
        fclose(f);
    }

    /* preflop mode needs the static rank table (next to the exe) */
    if (nboard == 0) {
        char path[1024] = "plo5rank.bin";
#ifdef _WIN32
        char exe[1024];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof path, "%s\\plo5rank.bin", exe);
        }
#endif
        plo5_ranks_load(path);
    }

    char cs[3];
    printf("hero  ");
    for (int i = 0; i < 5; i++) { plo5_card_str(hero[i], cs); printf("%s", cs); }
    if (nboard) {
        printf("   board  ");
        for (int i = 0; i < nboard; i++) { plo5_card_str(board[i], cs); printf("%s", cs); }
    } else {
        printf("   preflop");
    }
    printf("   pot %g   %d villain%s (%d-way)   %lluk trials/row\n",
           pot, nopp, nopp == 1 ? "" : "s", nopp + 1,
           (unsigned long long)(trials / 1000));
    if (ncombos)
        printf("villain range: %d combos from %s, MDF-trimmed per SPR\n",
               ncombos, rangefile);
    else
        printf("villain range: %.0f-%.0f percentile, MDF-trimmed per SPR\n",
               vlo, vhi);
    printf("\n  SPR     Stack   FinalPot    MDF   band (pct)  eq_need  hero_eq"
           "        no-fold  brkev_fold\n");

    int rc = PLO5_OK;
    if (ncombos) {
        /* build the board ranking once, score the combos, then per SPR
         * keep the top MDF fraction and evaluate against the band the
         * survivors span */
        if (nboard >= 3)
            rc = plo5_board_ranks_build(board, nboard, 100, nthreads, NULL, NULL);
        else if (!plo5_ranks_loaded())
            rc = PLO5_ERR_RANKS;
        double *scores = NULL;
        unsigned char *keep = NULL;
        if (rc == PLO5_OK) {
            scores = malloc((size_t)ncombos * sizeof *scores);
            keep = malloc((size_t)ncombos);
            for (int i = 0; i < ncombos && rc == PLO5_OK; i++) {
                scores[i] = plo5_hand_percentile_on(combos[i],
                                                    nboard ? board : NULL, nboard);
                if (scores[i] < 0) {
                    fprintf(stderr, "error: combo %d conflicts with the board\n", i + 1);
                    rc = PLO5_ERR_CARD;
                }
            }
        }
        for (int i = 0; i < nspr && rc == PLO5_OK; i++) {
            int nkeep = plo5_mdf_trim_scores(scores, ncombos,
                                             plo5_spr_mdf(sprs[i]), keep);
            double lo = 101, hi = -1;
            for (int k = 0; k < ncombos; k++)
                if (keep[k]) {
                    if (scores[k] < lo) lo = scores[k];
                    if (scores[k] > hi) hi = scores[k];
                }
            if (nkeep == 0) { lo = vhi; hi = vhi; }
            /* same fallback as plo5_spr_table: widen the floor when
             * nopp disjoint hands cannot be dealt from the band */
            double floor_lo = 101;
            for (int k = 0; k < ncombos; k++)
                if (scores[k] < floor_lo) floor_lo = scores[k];
            plo5_spr_row row;
            int widened = 0;
            for (;;) {
                rc = plo5_spr_row_eval(hero, nboard ? board : NULL, nboard,
                                       lo, hi, nopp, pot, sprs[i], trials,
                                       seed + (uint64_t)i, nthreads, &row);
                if (rc != PLO5_ERR_RANGE || lo <= floor_lo + 1e-9) break;
                lo = lo - 5.0 > floor_lo ? lo - 5.0 : floor_lo;
                widened = 1;
            }
            row.band_widened = widened;
            if (rc == PLO5_OK) print_row(&row, pot);
        }
        free(scores);
        free(keep);
    } else {
        plo5_spr_row *rows = malloc((size_t)nspr * sizeof *rows);
        rc = plo5_spr_table(hero, nboard ? board : NULL, nboard,
                            vlo, vhi, nopp, pot, sprs, nspr,
                            trials, seed, nthreads, rows);
        if (rc == PLO5_OK)
            for (int i = 0; i < nspr; i++) print_row(&rows[i], pot);
        free(rows);
    }
    free(combos);

    if (rc != PLO5_OK) {
        fprintf(stderr, "error: %s\n", err_str(rc));
        return 1;
    }
    printf("\nno-fold = stacking off is +EV with zero fold equity "
           "(hero_eq >= eq_need)\nbrkev_fold = villain fold%% that makes "
           "shoving breakeven when it isn't\n* = band floor relaxed below "
           "the MDF cut (that many disjoint hands from\n    the tight band "
           "cannot be dealt); the band shown is the one used\n");
    return 0;
}
