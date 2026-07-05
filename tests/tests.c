/* tests.c — unit tests for the PLO5 equity library. */
#include "plo5.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nfail = 0;

#define CHECK(cond, name) do {                                   \
    if (cond) printf("PASS  %s\n", name);                        \
    else      { printf("FAIL  %s\n", name); nfail++; }           \
} while (0)

static void cards_from(const char *s, int *out, int n)
{
    int k = 0;
    char b[3] = {0};
    for (; *s && k < n; s++) {
        if (*s == ' ') continue;
        b[0] = s[0]; b[1] = s[1]; s++;
        out[k] = plo5_parse_card(b);
        if (out[k] < 0) { fprintf(stderr, "bad test card %s\n", b); exit(2); }
        k++;
    }
    if (k != n) { fprintf(stderr, "bad test hand %s\n", s); exit(2); }
}

static uint32_t ev(const char *s)
{
    int c[5];
    cards_from(s, c, 5);
    return plo5_eval5(c);
}

/* ---- 5-card evaluator ordering ---- */
static void test_eval5_order(void)
{
    CHECK(ev("AsKsQsJsTs") > ev("AcAdAhAsKc"), "royal flush > quad aces");
    CHECK(ev("AcAdAhAsKc") > ev("AcAdAhKcKd"), "quads > full house");
    CHECK(ev("AcAdAhKcKd") > ev("AhKhQh9h2h"), "full house > flush");
    CHECK(ev("7h5h4h3h2h") > ev("AcKdQhJsTc"), "any flush > any straight");
    CHECK(ev("Ac2d3h4s5c") > ev("AcAdAhKcQd"), "wheel straight > trip aces");
    CHECK(ev("2c3d4h5s6c") > ev("Ac2d3h4s5c"), "6-high straight > wheel");
    CHECK(ev("AcAdAhKcQd") > ev("AcAdKhKcQd"), "trips > two pair");
    CHECK(ev("AcAdKhKcQd") > ev("AcAdKhQcJd"), "two pair > one pair");
    CHECK(ev("AcAdKhQcJd") > ev("AcKdQhJs9c"), "one pair > high card");
    CHECK(ev("AcAdKhQcJd") > ev("AcAdKhQc9d"), "pair kicker: AAKQJ > AAKQ9");
    CHECK(ev("KcKdQhQc2d") > ev("KcKdJhJcAd"), "two pair: KKQQ2 > KKJJA");
    CHECK(ev("AcKdQhJs9c") > ev("AcKdQhJs8c"), "high card kicker");
    CHECK(ev("2h3h4h5h6h") > ev("AcAdAhAsKc"), "straight flush > quads");
    CHECK(ev("As2h3h4h5h") == ev("Ad2c3c4c5c"), "suit-blind: wheel == wheel");
    CHECK(ev("Ah2h3h4h5h") > ev("KcKdKhKsAc"), "steel wheel > quad kings");
}

/* ---- exhaustive: all C(52,5) hands must fall into exactly 7462 classes ---- */
static int u32cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void test_eval5_classes(void)
{
    uint32_t *vals = malloc(2598960u * sizeof(uint32_t));
    if (!vals) { printf("FAIL  classes (oom)\n"); nfail++; return; }
    size_t n = 0;
    int c[5];
    for (c[0] = 0; c[0] < 48; c[0]++)
    for (c[1] = c[0] + 1; c[1] < 49; c[1]++)
    for (c[2] = c[1] + 1; c[2] < 50; c[2]++)
    for (c[3] = c[2] + 1; c[3] < 51; c[3]++)
    for (c[4] = c[3] + 1; c[4] < 52; c[4]++)
        vals[n++] = plo5_eval5(c);
    qsort(vals, n, sizeof(uint32_t), u32cmp);
    size_t classes = n ? 1 : 0;
    for (size_t i = 1; i < n; i++)
        if (vals[i] != vals[i - 1]) classes++;
    free(vals);
    CHECK(classes == 7462, "exactly 7462 distinct hand classes");
}

/* ---- Omaha rules: exactly 2 from hand + 3 from board ---- */
static void test_omaha_two_card_rule(void)
{
    /* Board is a royal flush. A player may NOT play the board: P1's two low
     * hearts still make the best flush; P2's JT only makes a straight. */
    int hands[10], board[5];
    cards_from("2h 3h 2c 2d 3c", hands, 5);
    cards_from("Jc Tc 9d 8d 2s", hands + 5, 5);
    cards_from("Ah Kh Qh Jh Th", board, 5);
    plo5_result r;
    int rc = plo5_equity(hands, 2, board, 5, NULL, 0, 0, UINT64_MAX, 1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact &&
          fabs(r.equity[0] - 1.0) < 1e-12 && fabs(r.equity[1]) < 1e-12,
          "board-royal: must use exactly 2 hole cards");
}

/* ---- nut hand on the river gets 100% ---- */
static void test_nut_river(void)
{
    int hands[10], board[5];
    cards_from("As Ks 2c 2h 3d", hands, 5);
    cards_from("Ac Ad 8h 8s 9c", hands + 5, 5);
    cards_from("Qs Js Ts 2d 7h", board, 5);
    plo5_result r;
    int rc = plo5_equity(hands, 2, board, 5, NULL, 0, 0, UINT64_MAX, 1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact &&
          fabs(r.equity[0] - 1.0) < 1e-12 && r.win[0] == 1.0 && r.tie[0] == 0.0,
          "river nuts (royal) = 100% equity");
}

/* ---- suit-symmetric matchup must be exactly 50/50 (exact enumeration) ---- */
static void test_symmetry(void)
{
    /* Swap clubs<->hearts and diamonds<->spades: P1 maps to P2 and the
     * board maps to itself, so exact equities must be identical. */
    int hands[10], board[4];
    cards_from("Ac Ad Kc Kd 5c", hands, 5);
    cards_from("Ah As Kh Ks 5h", hands + 5, 5);
    cards_from("2c 2h 7s 7d", board, 4);
    plo5_result r;
    int rc = plo5_equity(hands, 2, board, 4, NULL, 0, 0, UINT64_MAX, 1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact && r.samples == 38 &&
          fabs(r.equity[0] - 0.5) < 1e-12 && fabs(r.equity[1] - 0.5) < 1e-12,
          "suit-isomorphic hands split exactly 50/50");
}

/* ---- equities sum to 1 (3-way Monte Carlo) ---- */
static void test_sum_to_one(void)
{
    int hands[15];
    cards_from("Ah Kh Qd Jc Tc", hands, 5);
    cards_from("9s 8s 7d 6h 5c", hands + 5, 5);
    cards_from("2c 2d 3s 3h 4d", hands + 10, 5);
    plo5_result r;
    int rc = plo5_equity(hands, 3, NULL, 0, NULL, 0, 100000, 0, 7, 2, &r);
    double s = r.equity[0] + r.equity[1] + r.equity[2];
    CHECK(rc == PLO5_OK && fabs(s - 1.0) < 1e-9, "3-way equities sum to 1");
}

/* ---- Monte Carlo agrees with exact enumeration ---- */
static void test_mc_vs_exact(void)
{
    int hands[10], board[3];
    cards_from("Ah Kh Qd Jc Tc", hands, 5);
    cards_from("9s 8s 7d 6h 5c", hands + 5, 5);
    cards_from("2h 3h 4d", board, 3);
    plo5_result ex, mc;
    int rc1 = plo5_equity(hands, 2, board, 3, NULL, 0, 0, UINT64_MAX, 1, 1, &ex);
    int rc2 = plo5_equity(hands, 2, board, 3, NULL, 0, 500000, 0, 42, 2, &mc);
    int ok = rc1 == PLO5_OK && rc2 == PLO5_OK && ex.exact && !mc.exact &&
             ex.samples == 741 &&
             fabs(ex.equity[0] - mc.equity[0]) < 0.005 &&
             fabs(ex.equity[1] - mc.equity[1]) < 0.005;
    CHECK(ok, "Monte Carlo within 0.5% of exact (741 runouts)");
    /* exact result should also be inside MC's 95% CI (allow 3x for slack) */
    CHECK(ok && fabs(ex.equity[0] - mc.equity[0]) < 3 * mc.ci95[0] + 1e-9,
          "exact equity inside Monte Carlo confidence interval");
}

/* ---- random-hand opponent + input validation ---- */
static void test_misc(void)
{
    int hands[10];
    cards_from("Ah Ad Kh Kd 7s", hands, 5);
    for (int i = 0; i < 5; i++) hands[5 + i] = PLO5_RANDOM;
    plo5_result r;
    int rc = plo5_equity(hands, 2, NULL, 0, NULL, 0, 200000, 1000000, 3, 2, &r);
    CHECK(rc == PLO5_OK && !r.exact &&
          r.equity[0] > 0.55 && r.equity[0] < 0.95,
          "double-suited aces beat a random hand");

    int dup[10];
    cards_from("Ah Ad Kh Kd 7s", dup, 5);
    cards_from("Ah 2c 3c 4c 5c", dup + 5, 5);   /* Ah reused */
    CHECK(plo5_equity(dup, 2, NULL, 0, NULL, 0, 100, 0, 1, 1, &r)
              == PLO5_ERR_DUP, "duplicate card rejected");

    CHECK(plo5_parse_card("Xx") == -1 && plo5_parse_card("A") == -1 &&
          plo5_parse_card("ah") == plo5_parse_card("Ah"),
          "card parsing edge cases");
}

/* Omaha value of hand on a complete board, via the public evaluator */
static uint32_t omaha_val(const int hand[5], const int board[5])
{
    static const int PIX[10][2] = {
        {0,1},{0,2},{0,3},{0,4},{1,2},{1,3},{1,4},{2,3},{2,4},{3,4}
    };
    static const int TIX[10][3] = {
        {0,1,2},{0,1,3},{0,1,4},{0,2,3},{0,2,4},
        {0,3,4},{1,2,3},{1,2,4},{1,3,4},{2,3,4}
    };
    uint32_t best = 0;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++) {
            int c[5] = { hand[PIX[i][0]], hand[PIX[i][1]],
                         board[TIX[j][0]], board[TIX[j][1]], board[TIX[j][2]] };
            uint32_t v = plo5_eval5(c);
            if (v > best) best = v;
        }
    return best;
}

/* ---- preflop percentile table (needs plo5rank.bin) ---- */
static void test_preflop_ranks(void)
{
    if (plo5_ranks_load("plo5rank.bin") != PLO5_OK) {
        printf("SKIP  preflop rank tests (plo5rank.bin not found)\n");
        return;
    }
    int h[5];
    plo5_percentile_hand(30, h);
    double p = plo5_hand_percentile(h);
    CHECK(fabs(p - 30.0) < 0.01, "preflop percentile round-trip at 30");

    int aa[5];
    cards_from("Ah Ad Kh Kd 7s", aa, 5);
    CHECK(plo5_hand_percentile(aa) > 97.0, "AAKK double-suited above 97th pct");

    /* strong percentile hand beats random much more often than a weak one */
    plo5_player pl[2];
    memset(pl, 0, sizeof pl);
    pl[1].type = PLO5_P_RANDOM;
    plo5_result r95, r5;
    pl[0].type = PLO5_P_FIXED;
    plo5_percentile_hand(95, pl[0].cards);
    plo5_equity2(pl, 2, NULL, 0, NULL, 0, 100000, 0, 11, 2, &r95);
    plo5_percentile_hand(5, pl[0].cards);
    plo5_equity2(pl, 2, NULL, 0, NULL, 0, 100000, 0, 11, 2, &r5);
    CHECK(r95.equity[0] > 0.55 && r5.equity[0] < 0.48 &&
          r95.equity[0] > r5.equity[0] + 0.10,
          "95th pct hand beats random, 5th pct hand loses to it");

    /* preflop range: top decile crushes a random hand */
    pl[0].type = PLO5_P_RANGE;
    pl[0].lo = 90; pl[0].hi = 100;
    plo5_result rr;
    int rc = plo5_equity2(pl, 2, NULL, 0, NULL, 0, 100000, 0, 12, 2, &rr);
    CHECK(rc == PLO5_OK && rr.equity[0] > 0.55, "90-100 pct range beats random");
}

/* ---- board-conditional rankings ---- */
static void test_board_ranks(void)
{
    int river[5];
    cards_from("Kh 7d 2s 8c 3h", river, 5);
    int rc = plo5_board_ranks_build(river, 5, 0, 4, NULL, NULL);
    int h10[5], h50[5], h90[5];
    plo5_percentile_hand_on(10, river, 5, h10);
    plo5_percentile_hand_on(50, river, 5, h50);
    plo5_percentile_hand_on(90, river, 5, h90);
    CHECK(rc == PLO5_OK &&
          omaha_val(h10, river) <= omaha_val(h50, river) &&
          omaha_val(h50, river) <= omaha_val(h90, river),
          "river percentiles ordered by made-hand strength");
    double p = plo5_hand_percentile_on(h50, river, 5);
    CHECK(fabs(p - 50.0) < 0.5, "river percentile round-trip at 50");

    /* a river-conditional 99th pct hand must beat a 40th pct hand there */
    int h99[5], h40[5];
    plo5_percentile_hand_on(99, river, 5, h99);
    plo5_percentile_hand_on(40, river, 5, h40);
    CHECK(omaha_val(h99, river) > omaha_val(h40, river),
          "river 99th pct strictly beats 40th pct");

    /* flop: strong band vs weak band */
    int flop[3];
    cards_from("Kh 7d 2s", flop, 3);
    plo5_player pl[2];
    memset(pl, 0, sizeof pl);
    pl[0].type = PLO5_P_RANGE; pl[0].lo = 80; pl[0].hi = 100;
    pl[1].type = PLO5_P_RANGE; pl[1].lo = 0;  pl[1].hi = 40;
    plo5_result r;
    rc = plo5_equity2(pl, 2, flop, 3, NULL, 0, 100000, 0, 13, 4, &r);
    CHECK(rc == PLO5_OK && r.equity[0] > 0.60,
          "80-100 flop range beats 0-40 flop range");

    /* preflop table still intact after board builds */
    if (plo5_ranks_loaded()) {
        int hh[5];
        plo5_percentile_hand(99, hh);
        CHECK(fabs(plo5_hand_percentile(hh) - 99.0) < 0.01,
              "preflop table unaffected by board builds");
    }
}

/* ---- double board, split pot ---- */
static void test_double_board(void)
{
    plo5_player pl[3];
    memset(pl, 0, sizeof pl);
    int bA[5], bB[5];
    cards_from("Qs Js Ts 2d 7h", bA, 5);
    cards_from("Qc Jc Tc 3d 8h", bB, 5);
    plo5_result r;

    /* nuts on both boards = scoop the whole pot */
    pl[0].type = PLO5_P_FIXED;
    pl[1].type = PLO5_P_FIXED;
    cards_from("As Ks Ac Kc 2h", pl[0].cards, 5);
    cards_from("9s 9h 8d 8s 4c", pl[1].cards, 5);
    int rc = plo5_equity_2b(pl, 2, bA, 5, bB, 5, NULL, 0, 0, UINT64_MAX,
                            1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact && r.samples == 1 &&
          fabs(r.equity[0] - 1.0) < 1e-12 && r.win[0] == 1.0,
          "double board: nuts on both = full scoop");

    /* one board each = exactly half the pot, no scoops */
    cards_from("As Ks 2c 2s 3s", pl[0].cards, 5);   /* royal on A only */
    cards_from("Ac Kc 4d 4h 5d", pl[1].cards, 5);   /* royal on B only */
    rc = plo5_equity_2b(pl, 2, bA, 5, bB, 5, NULL, 0, 0, UINT64_MAX, 1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact &&
          fabs(r.equity[0] - 0.5) < 1e-12 && fabs(r.equity[1] - 0.5) < 1e-12 &&
          r.win[0] == 0.0 && r.win[1] == 0.0 &&
          r.tie[0] == 1.0 && r.tie[1] == 1.0,
          "double board: one board each = exactly half pot");

    /* turn/turn: exact enumeration counts n*(n-1) ordered river pairs */
    int tA[4], tB[4];
    cards_from("Qs Js Ts 2d", tA, 4);
    cards_from("Qc Jc Tc 3d", tB, 4);
    rc = plo5_equity_2b(pl, 2, tA, 4, tB, 4, NULL, 0, 0, UINT64_MAX, 1, 1, &r);
    CHECK(rc == PLO5_OK && r.exact && r.samples == 34ull * 33ull,
          "double board turn/turn enumerates 34*33 runouts");

    /* preflop double board Monte Carlo: equities sum to 1 */
    pl[2].type = PLO5_P_FIXED;
    cards_from("Ah Kh Qd Jc Tc", pl[0].cards, 5);
    cards_from("9s 8s 7d 6h 5c", pl[1].cards, 5);
    cards_from("2c 2d 3s 3h 4d", pl[2].cards, 5);
    rc = plo5_equity_2b(pl, 3, NULL, 0, NULL, 0, NULL, 0, 50000, 0, 7, 2, &r);
    double s = r.equity[0] + r.equity[1] + r.equity[2];
    CHECK(rc == PLO5_OK && !r.exact && fabs(s - 1.0) < 1e-9,
          "double board 3-way equities sum to 1");

    /* double-board ranges: strong band beats weak band on two flops */
    if (plo5_ranks_loaded()) {
        int fA[3], fB[3];
        cards_from("Kh 7d 2s", fA, 3);
        cards_from("9c 8c 4h", fB, 3);
        memset(pl, 0, sizeof pl);
        pl[0].type = PLO5_P_RANGE; pl[0].lo = 80; pl[0].hi = 100;
        pl[1].type = PLO5_P_RANGE; pl[1].lo = 0;  pl[1].hi = 40;
        rc = plo5_equity_2b(pl, 2, fA, 3, fB, 3, NULL, 0, 60000, 0, 8, 4, &r);
        CHECK(rc == PLO5_OK && r.equity[0] > 0.60,
              "double-board 80-100 range beats 0-40 range");
    }
}

int main(void)
{
    plo5_init();
    test_eval5_order();
    test_eval5_classes();
    test_omaha_two_card_rule();
    test_nut_river();
    test_symmetry();
    test_sum_to_one();
    test_mc_vs_exact();
    test_misc();
    test_preflop_ranks();
    test_board_ranks();
    test_double_board();

    /* category-count test reuses the big enumeration; run it last */
    {
        static const long long expect[9] = {
            1302540, 1098240, 123552, 54912, 10200, 5108, 3744, 624, 40
        };
        long long got[9] = {0};
        int c[5];
        for (c[0] = 0; c[0] < 48; c[0]++)
        for (c[1] = c[0] + 1; c[1] < 49; c[1]++)
        for (c[2] = c[1] + 1; c[2] < 50; c[2]++)
        for (c[3] = c[2] + 1; c[3] < 51; c[3]++)
        for (c[4] = c[3] + 1; c[4] < 52; c[4]++)
            got[plo5_eval5(c) >> 24]++;
        int ok = 1;
        for (int i = 0; i < 9; i++) if (got[i] != expect[i]) ok = 0;
        CHECK(ok, "category counts over all 2,598,960 hands");
    }

    printf("\n%s (%d failure%s)\n", nfail ? "FAILED" : "ALL TESTS PASSED",
           nfail, nfail == 1 ? "" : "s");
    return nfail ? 1 : 0;
}
