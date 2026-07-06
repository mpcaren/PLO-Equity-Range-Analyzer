/* main.c — CLI for the PLO5 equity library.
 *
 * plo5calc "AhKhQd Jc Tc" "9s 8s 7d 6h 5c" --board "2h 3h 4d" --trials 1000000
 * plo5calc "AhAdKhKd7s" "10-40" --trials 500000        (percentile range)
 * plo5calc --gen-ranks                                  (build plo5rank.bin)
 * plo5calc --percentile 30                              (30th pct hand)
 * plo5calc --rank-of AhAdKhKd7s                         (hand -> percentile)
 */
#include "plo5.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static double now_sec(void)
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

static int ncpus(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* default rank-table path: next to the executable, else cwd */
static void default_rank_path(char *buf, size_t n)
{
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)n);
    if (len > 0 && len < n) {
        char *slash = strrchr(buf, '\\');
        if (slash && (size_t)(slash - buf) + 14 < n) {
            strcpy(slash + 1, "plo5rank.bin");
            return;
        }
    }
#endif
    snprintf(buf, n, "plo5rank.bin");
}

/* parse "Ah Kh,Qd" style card lists (whitespace/commas ignored);
 * returns count or -1 */
static int parse_cards(const char *s, int *out, int maxn)
{
    int n = 0;
    char buf[2];
    int bi = 0;
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

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* parse one "lo-hi" or "lo-hi%" segment; returns 0 or -1 */
static int parse_band(const char *s, const char *end_at, double *lo, double *hi)
{
    char buf[32], *e = NULL;
    size_t n = (size_t)(end_at - s);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (!isdigit((unsigned char)buf[0])) return -1;
    *lo = strtod(buf, &e);
    if (!e || *e != '-') return -1;
    *hi = strtod(e + 1, &e);
    if (e && *e == '%') e++;
    if (e && *e != 0) return -1;
    if (*lo < 0 || *hi > 100 || *hi <= *lo) return -1;
    return 0;
}

/* "random"/"*" -> random; "10-40" (percentiles, 0=weakest) -> range;
 * "0-50>0-60>40-100" -> a range that CONTINUES across streets: keep this
 * percentile slice of the flop, then of THOSE survivors keep this slice
 * on the turn, then the final segment is the band on the current board;
 * 2 segments = flop-continuation only, 1 = plain range. Otherwise 5
 * cards. Returns 0 or -1. */
static int parse_player(const char *s, plo5_player *pl)
{
    memset(pl, 0, sizeof *pl);
    if (str_ieq(s, "random") || str_ieq(s, "*")) {
        pl->type = PLO5_P_RANDOM;
        return 0;
    }
    if (isdigit((unsigned char)s[0]) && strchr(s, '-')) {
        const char *seg[1 + PLO5_CHAIN_MAX + 1];
        int nseg = 0;
        const char *p = s;
        for (;;) {
            const char *gt = strchr(p, '>');
            if (nseg >= (int)(sizeof seg / sizeof seg[0])) return -1;
            seg[nseg++] = gt ? gt : p + strlen(p);
            if (!gt) break;
            p = gt + 1;
        }
        if (nseg > PLO5_CHAIN_MAX + 1) return -1;
        double lo[PLO5_CHAIN_MAX + 1], hi[PLO5_CHAIN_MAX + 1];
        const char *segstart = s;
        for (int i = 0; i < nseg; i++) {
            if (parse_band(segstart, seg[i], &lo[i], &hi[i]) != 0) return -1;
            segstart = seg[i] + 1;
        }
        pl->type = PLO5_P_RANGE;
        pl->chain_n = nseg - 1;
        for (int i = 0; i < pl->chain_n; i++) {
            pl->chain_lo[i] = lo[i];
            pl->chain_hi[i] = hi[i];
        }
        pl->lo = lo[nseg - 1];
        pl->hi = hi[nseg - 1];
        return 0;
    }
    int n = parse_cards(s, pl->cards, 5);
    if (n != 5) return -1;
    pl->type = PLO5_P_FIXED;
    return 0;
}

static const char *err_str(int rc)
{
    switch (rc) {
    case PLO5_ERR_PLAYERS: return "need 2..6 players";
    case PLO5_ERR_BOARD:   return "board must have 0, 3, 4 or 5 cards";
    case PLO5_ERR_CARD:    return "invalid card";
    case PLO5_ERR_DUP:     return "duplicate card";
    case PLO5_ERR_DECK:    return "not enough cards left in deck";
    case PLO5_ERR_TRIALS:  return "trials is 0 and exact enumeration not possible";
    case PLO5_ERR_RANKS:   return "rank table not loaded - run plo5calc --gen-ranks first";
    case PLO5_ERR_IO:      return "cannot read/write rank file";
    case PLO5_ERR_RANGE:   return "range cannot be dealt with these dead cards";
    default:               return "invalid arguments";
    }
}

static void player_label(const plo5_player *pl, char out[48])
{
    if (pl->type == PLO5_P_RANDOM) { strcpy(out, "random"); return; }
    if (pl->type == PLO5_P_RANGE) {
        out[0] = 0;
        for (int i = 0; i < pl->chain_n; i++) {
            char seg[16];
            snprintf(seg, 16, "%g-%g>", pl->chain_lo[i], pl->chain_hi[i]);
            strcat(out, seg);
        }
        char seg[24];
        snprintf(seg, 24, "%g-%g%%", pl->lo, pl->hi);
        strcat(out, seg);
        return;
    }
    char c[3];
    out[0] = 0;
    for (int i = 0; i < 5; i++) {
        plo5_card_str(pl->cards[i], c);
        strcat(out, c);
    }
}

static uint64_t parse_u64(const char *s)
{
    char *end = NULL;
    uint64_t v = strtoull(s, &end, 10);
    if (end) {
        if (*end == 'k' || *end == 'K') v *= 1000ull;
        else if (*end == 'm' || *end == 'M') v *= 1000000ull;
    }
    return v;
}

static void usage(void)
{
    printf(
"plo5calc — 5-card PLO equity calculator\n"
"\n"
"usage: plo5calc HAND HAND [HAND...] [options]     (2..6 hands)\n"
"       plo5calc --batch FILE [options]\n"
"       plo5calc --gen-ranks | --percentile P | --rank-of HAND\n"
"       plo5calc --bench\n"
"\n"
"HAND is one of:\n"
"  5 cards          \"AhKhQdJcTc\" (spaces ok inside quotes)\n"
"  random (or *)    a random hand each trial\n"
"  LO-HI            percentile band of hand strength, e.g. 10-40\n"
"                   (0 = weakest, 100 = strongest; needs rank table)\n"
"  LO-HI>LO-HI      a range that CONTINUES across streets: keep this\n"
"                   slice of the flop, then keep this slice of THOSE\n"
"                   survivors on the turn/river (\"only x%% of the hands\n"
"                   from the previous street\"). Up to 2 continuations,\n"
"                   e.g. \"0-50>0-60>40-100\" = flop top 50%%, of those\n"
"                   the top 40%% on the turn, of those 40-100%% on the\n"
"                   river. Single-board mode only.\n"
"\n"
"options:\n"
"  --board CARDS    0, 3, 4 or 5 board cards, e.g. \"2h 3h 4d\"\n"
"  --double         double-board split pot: two boards, each pays half;\n"
"                   best on both scoops (win%% = clean scoops)\n"
"  --board2 CARDS   second board (implies --double)\n"
"  --dead CARDS     dead cards removed from the deck\n"
"  --trials N       Monte Carlo trials (default 1000000; k/m suffix ok)\n"
"  --seed N         PRNG seed (default: time-based)\n"
"  --threads N      worker threads, 0 = all cores (default 1)\n"
"  --exact          force exact enumeration when possible\n"
"  --mc             force Monte Carlo\n"
"  --max-enum N     enumerate exactly when runouts <= N (default 1000000)\n"
"  --batch FILE     one matchup per line: hands + optional board:CARDS\n"
"  --ranks FILE     rank table path (default: plo5rank.bin next to exe)\n"
"\n"
"rank table (hand-strength percentiles):\n"
"  --gen-ranks      build the table: every starting hand ranked by\n"
"                   equity vs a random hand (takes a few minutes)\n"
"  --rank-trials N  MC trials per hand class for --gen-ranks (default 25000)\n"
"  --percentile P   print the hand at percentile P (0..100)\n"
"  --rank-of HAND   print the percentile of a specific hand\n"
"    (both honor --board: the distribution is then conditional on the\n"
"     given flop/turn/river instead of the preflop table)\n"
"  --runouts N      sampled runouts for flop rankings (default 100)\n"
"  --bench          run a performance benchmark\n");
}

static int g_runouts = 100;

static void board_progress(int done, int total, void *ud)
{
    (void)ud;
    fprintf(stderr, "\r  ranking board distribution: %d / %d   ", done, total);
}

static int run_one(const plo5_player *pl, int nh, const int *board, int nb,
                   const int *board2, int nb2, int db,
                   const int *dead, int nd, uint64_t trials, uint64_t max_enum,
                   uint64_t seed, int threads, int quiet_csv)
{
    plo5_result r;
    int has_range = 0;
    for (int p = 0; p < nh; p++)
        if (pl[p].type == PLO5_P_RANGE) has_range = 1;
    if (has_range && db && (nb > 0) != (nb2 > 0)) {
        fprintf(stderr, "error: double-board ranges need both boards set "
                        "(or neither)\n");
        return 1;
    }
    if (has_range && nb > 0) {
        int nt = threads > 1 ? threads : ncpus();
        int rc2 = db
            ? plo5_board_ranks_build2(board, nb, board2, nb2, g_runouts, nt,
                                      quiet_csv ? NULL : board_progress, NULL)
            : plo5_board_ranks_build(board, nb, g_runouts, nt,
                                     quiet_csv ? NULL : board_progress, NULL);
        if (!quiet_csv) fprintf(stderr, "\n");
        if (rc2 != PLO5_OK) {
            fprintf(stderr, "error: %s\n", err_str(rc2));
            return 1;
        }
    }
    int rc = db
        ? plo5_equity_2b(pl, nh, board, nb, board2, nb2, dead, nd,
                         trials, max_enum, seed, threads, &r)
        : plo5_equity2(pl, nh, board, nb, dead, nd,
                       trials, max_enum, seed, threads, &r);
    if (rc != PLO5_OK) {
        fprintf(stderr, "error: %s\n", err_str(rc));
        return 1;
    }

    char lbl[48], c[3];

    if (quiet_csv) {
        for (int p = 0; p < nh; p++) {
            player_label(&pl[p], lbl);
            printf("%s,", lbl);
        }
        for (int p = 0; p < nh; p++)
            printf("%.6f%s", r.equity[p], p == nh - 1 ? "\n" : ",");
        return 0;
    }

    if (db) printf("Mode  : double board, split pot\n");
    if (nb > 0) {
        printf(db ? "BoardA: " : "Board : ");
        for (int i = 0; i < nb; i++) {
            plo5_card_str(board[i], c);
            printf("%s ", c);
        }
        printf("\n");
    }
    if (db && nb2 > 0) {
        printf("BoardB: ");
        for (int i = 0; i < nb2; i++) {
            plo5_card_str(board2[i], c);
            printf("%s ", c);
        }
        printf("\n");
    }
    if (nd > 0) {
        printf("Dead  : ");
        for (int i = 0; i < nd; i++) {
            plo5_card_str(dead[i], c);
            printf("%s ", c);
        }
        printf("\n");
    }
    if (r.exact)
        printf("Method: exact enumeration, %" PRIu64 " runouts\n\n", r.samples);
    else
        printf("Method: Monte Carlo, %" PRIu64 " trials, seed %" PRIu64
               ", %d thread(s)\n\n", r.samples, seed, threads);

    for (int p = 0; p < nh; p++) {
        player_label(&pl[p], lbl);
        printf("Player %d  %-20s  equity %7.3f%%   win %7.3f%%  tie %6.3f%%",
               p + 1, lbl, r.equity[p] * 100.0, r.win[p] * 100.0,
               r.tie[p] * 100.0);
        if (!r.exact)
            printf("   +/- %.3f%%", r.ci95[p] * 100.0);
        if (pl[p].type == PLO5_P_FIXED) {
            double pc = db
                ? (nb >= 3 && nb2 >= 3
                       ? plo5_hand_percentile_2b(pl[p].cards, board, nb,
                                                 board2, nb2)
                       : (nb == 0 && nb2 == 0
                              ? plo5_hand_percentile_on(pl[p].cards, NULL, 0)
                              : -1.0))
                : plo5_hand_percentile_on(pl[p].cards, board, nb);
            if (pc >= 0)
                printf("   [pct %.1f%s]", pc,
                       db && nb >= 3 ? " both boards" :
                       !db && nb > 0 ? " on board" : "");
        }
        printf("\n");
    }
    return 0;
}

static int run_batch(const char *path, int dbl, uint64_t trials,
                     uint64_t max_enum, uint64_t seed, int threads)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return 1;
    }
    char line[1024];
    int lineno = 0, errors = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        plo5_player pl[PLO5_MAX_PLAYERS];
        int nh = 0;
        int board[5], nb = 0, board2[5], nb2 = 0, bad = 0, ldbl = dbl;

        for (char *tok = strtok(line, " \t\r\n"); tok;
             tok = strtok(NULL, " \t\r\n")) {
            if (strncmp(tok, "board:", 6) == 0) {
                nb = parse_cards(tok + 6, board, 5);
                if (nb < 0) { bad = 1; break; }
            } else if (strncmp(tok, "board2:", 7) == 0) {
                nb2 = parse_cards(tok + 7, board2, 5);
                if (nb2 < 0) { bad = 1; break; }
                ldbl = 1;
            } else if (str_ieq(tok, "double")) {
                ldbl = 1;
            } else {
                if (nh >= PLO5_MAX_PLAYERS) { bad = 1; break; }
                if (parse_player(tok, &pl[nh]) != 0) { bad = 1; break; }
                nh++;
            }
        }
        if (nh == 0 && !bad) continue;   /* blank / comment-only line */
        if (bad || nh < 2) {
            fprintf(stderr, "line %d: skipped (parse error or <2 hands)\n",
                    lineno);
            errors++;
            continue;
        }
        if (run_one(pl, nh, board, nb, board2, nb2, ldbl, NULL, 0,
                    trials, max_enum, seed + (uint64_t)lineno, threads, 1) != 0)
            errors++;
        fflush(stdout);
    }
    fclose(f);
    return errors ? 1 : 0;
}

static int run_bench(void)
{
    plo5_player pl[2];
    parse_player("AhAdKhKd7s", &pl[0]);
    parse_player("QsJsTs9h8c", &pl[1]);
    const uint64_t T = 2000000;
    plo5_result r;

    printf("Benchmark: AhAdKhKd7s vs QsJsTs9h8c, preflop, %" PRIu64
           " Monte Carlo trials\n", T);

    double t0 = now_sec();
    plo5_equity2(pl, 2, NULL, 0, NULL, 0, T, 0, 42, 1, &r);
    double dt1 = now_sec() - t0;
    printf("1 thread : %8.2fs  %10.0f trials/s  %11.0f evals/s   "
           "(P1 equity %.3f%%)\n",
           dt1, (double)T / dt1, (double)T * 200.0 / dt1,
           r.equity[0] * 100.0);

    int nc = ncpus();
    t0 = now_sec();
    plo5_equity2(pl, 2, NULL, 0, NULL, 0, T, 0, 42, nc, &r);
    double dt2 = now_sec() - t0;
    printf("%d threads: %8.2fs  %10.0f trials/s  %11.0f evals/s\n",
           nc, dt2, (double)T / dt2, (double)T * 200.0 / dt2);
    return 0;
}

static void gen_progress(int done, int total, void *ud)
{
    (void)ud;
    fprintf(stderr, "\r  ranking hand classes: %d / %d (%.0f%%)   ",
            done, total, 100.0 * done / (total ? total : 1));
}

int main(int argc, char **argv)
{
    plo5_init();

    uint64_t trials = 1000000, max_enum = 1000000, seed = 0;
    uint32_t rank_trials = 25000;
    int threads = 1, dbl = 0;
    const char *board_s = NULL, *board2_s = NULL, *dead_s = NULL, *batch = NULL;
    const char *rank_of = NULL, *ranks_path_arg = NULL;
    const char *hand_s[PLO5_MAX_PLAYERS];
    int nh = 0, bench = 0, gen_ranks = 0;
    double pct_query = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "--board") == 0 && i + 1 < argc) {
            board_s = argv[++i];
        } else if (strcmp(a, "--board2") == 0 && i + 1 < argc) {
            board2_s = argv[++i];
            dbl = 1;
        } else if (strcmp(a, "--double") == 0) {
            dbl = 1;
        } else if (strcmp(a, "--dead") == 0 && i + 1 < argc) {
            dead_s = argv[++i];
        } else if (strcmp(a, "--trials") == 0 && i + 1 < argc) {
            trials = parse_u64(argv[++i]);
        } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            seed = parse_u64(argv[++i]);
        } else if (strcmp(a, "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
            if (threads <= 0) threads = ncpus();
        } else if (strcmp(a, "--exact") == 0) {
            max_enum = UINT64_MAX;
        } else if (strcmp(a, "--mc") == 0) {
            max_enum = 0;
        } else if (strcmp(a, "--max-enum") == 0 && i + 1 < argc) {
            max_enum = parse_u64(argv[++i]);
        } else if (strcmp(a, "--batch") == 0 && i + 1 < argc) {
            batch = argv[++i];
        } else if (strcmp(a, "--ranks") == 0 && i + 1 < argc) {
            ranks_path_arg = argv[++i];
        } else if (strcmp(a, "--gen-ranks") == 0) {
            gen_ranks = 1;
        } else if (strcmp(a, "--rank-trials") == 0 && i + 1 < argc) {
            rank_trials = (uint32_t)parse_u64(argv[++i]);
        } else if (strcmp(a, "--percentile") == 0 && i + 1 < argc) {
            pct_query = atof(argv[++i]);
        } else if (strcmp(a, "--rank-of") == 0 && i + 1 < argc) {
            rank_of = argv[++i];
        } else if (strcmp(a, "--runouts") == 0 && i + 1 < argc) {
            g_runouts = atoi(argv[++i]);
        } else if (strcmp(a, "--bench") == 0) {
            bench = 1;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "unknown option: %s (see --help)\n", a);
            return 1;
        } else {
            if (nh >= PLO5_MAX_PLAYERS) {
                fprintf(stderr, "error: at most %d hands\n", PLO5_MAX_PLAYERS);
                return 1;
            }
            hand_s[nh++] = a;
        }
    }

    char rank_path[512];
    if (ranks_path_arg)
        snprintf(rank_path, sizeof rank_path, "%s", ranks_path_arg);
    else
        default_rank_path(rank_path, sizeof rank_path);

    if (gen_ranks) {
        int nt = threads > 1 ? threads : ncpus();
        fprintf(stderr, "Building rank table (%u trials/class, %d threads)"
                " -> %s\n", rank_trials, nt, rank_path);
        double t0 = now_sec();
        int rc = plo5_ranks_generate(rank_path, rank_trials, nt, 20260704,
                                     gen_progress, NULL);
        fprintf(stderr, "\n");
        if (rc != PLO5_OK) {
            fprintf(stderr, "error: %s\n", err_str(rc));
            return 1;
        }
        fprintf(stderr, "done in %.0f s\n", now_sec() - t0);
        return 0;
    }

    /* load the rank table if present (needed for ranges / percentiles) */
    int ranks_ok = plo5_ranks_load(rank_path) == PLO5_OK;

    if (bench) return run_bench();

    if (pct_query >= 0 || rank_of) {
        int qb[5], qnb = 0;
        if (board_s) {
            qnb = parse_cards(board_s, qb, 5);
            if (qnb < 0 || (qnb != 0 && qnb != 3 && qnb != 4 && qnb != 5)) {
                fprintf(stderr, "error: bad --board\n");
                return 1;
            }
        }
        if (qnb == 0 && !ranks_ok) {
            fprintf(stderr, "error: %s\n", err_str(PLO5_ERR_RANKS));
            return 1;
        }
        if (qnb > 0) {
            int rc = plo5_board_ranks_build(qb, qnb, g_runouts, ncpus(),
                                            board_progress, NULL);
            fprintf(stderr, "\n");
            if (rc != PLO5_OK) {
                fprintf(stderr, "error: %s\n", err_str(rc));
                return 1;
            }
        }
        static const char *STREET[6] = { "preflop", "?", "?", "flop", "turn", "river" };
        if (pct_query >= 0) {
            int h[5];
            plo5_percentile_hand_on(pct_query, qb, qnb, h);
            char c[3];
            printf("percentile %.1f (%s): ", pct_query, STREET[qnb]);
            for (int i = 4; i >= 0; i--) { plo5_card_str(h[i], c); printf("%s", c); }
            printf("  (pct of that hand: %.2f)\n",
                   plo5_hand_percentile_on(h, qb, qnb));
        } else {
            int h[5];
            if (parse_cards(rank_of, h, 5) != 5) {
                fprintf(stderr, "error: cannot parse hand '%s'\n", rank_of);
                return 1;
            }
            double pc = plo5_hand_percentile_on(h, qb, qnb);
            if (pc < 0) {
                fprintf(stderr, "error: hand conflicts with the board\n");
                return 1;
            }
            printf("%s: percentile %.2f on %s (0 = weakest, 100 = strongest)\n",
                   rank_of, pc, STREET[qnb]);
        }
        return 0;
    }

    if (seed == 0)
        seed = (uint64_t)time(NULL) * 2654435761ull + (uint64_t)clock();

    if (batch) return run_batch(batch, dbl, trials, max_enum, seed, threads);

    if (nh < 2) {
        usage();
        return 1;
    }

    plo5_player pl[PLO5_MAX_PLAYERS];
    for (int p = 0; p < nh; p++)
        if (parse_player(hand_s[p], &pl[p]) != 0) {
            fprintf(stderr, "error: cannot parse hand '%s' "
                    "(5 cards like AhKhQdJcTc, 'random', or a band like 10-40)\n",
                    hand_s[p]);
            return 1;
        }

    int board[5], nb = 0;
    if (board_s) {
        nb = parse_cards(board_s, board, 5);
        if (nb < 0) {
            fprintf(stderr, "error: cannot parse board '%s'\n", board_s);
            return 1;
        }
    }
    int board2[5], nb2 = 0;
    if (board2_s) {
        nb2 = parse_cards(board2_s, board2, 5);
        if (nb2 < 0) {
            fprintf(stderr, "error: cannot parse board2 '%s'\n", board2_s);
            return 1;
        }
    }
    int dead[20], nd = 0;
    if (dead_s) {
        nd = parse_cards(dead_s, dead, 20);
        if (nd < 0) {
            fprintf(stderr, "error: cannot parse dead cards '%s'\n", dead_s);
            return 1;
        }
    }

    return run_one(pl, nh, board, nb, board2, nb2, dbl, dead, nd,
                   trials, max_enum, seed, threads, 0);
}
