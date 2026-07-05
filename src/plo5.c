/* plo5.c — PLO5 equity core: 5-card evaluator + Monte Carlo / exact enumeration.
 *
 * Evaluator: Cactus-Kev-style. Each card is a 32-bit key
 *     bit 16+rank : rank bit          (OR of 5 keys >> 16 = rank mask)
 *     bit 12+suit : suit bit          (AND of 5 keys & 0xF000 != 0 = flush)
 *     bits 0..5   : prime for rank    (product identifies the rank multiset)
 * Flushes and 5-distinct-rank hands are looked up by the 13-bit rank mask;
 * paired hands by the prime product through a small open-addressing hash.
 * All tables (~100 KB) are generated at init — no baked-in data files.
 *
 * Omaha: each player pre-combines their C(5,2)=10 two-card sets once; each
 * runout pre-combines the C(5,3)=10 board triples once; a hand evaluation is
 * then two ANDs/ORs, one multiply and a table probe.
 */
#include "plo5.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ */
/* Evaluator tables                                                    */
/* ------------------------------------------------------------------ */

enum {
    CAT_HIGH = 0, CAT_PAIR, CAT_TWOPAIR, CAT_TRIPS, CAT_STRAIGHT,
    CAT_FLUSH, CAT_FULL, CAT_QUADS, CAT_STFLUSH
};

#define HASH_SIZE 16384          /* 4888 keys -> ~30% load */
#define HASH_MASK (HASH_SIZE - 1)

static uint32_t flush_tbl[8192]; /* rank mask -> value, all 5 same suit  */
static uint32_t uniq5_tbl[8192]; /* rank mask -> value, 5 distinct ranks */
static uint32_t hkeys[HASH_SIZE];
static uint32_t hvals[HASH_SIZE];
static uint32_t card_key[52];
static int      initialized = 0;

static const uint32_t rank_prime[13] =
    { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41 };

static void hash_put(uint32_t key, uint32_t val)
{
    uint32_t i = (key * 0x9E3779B1u) >> 18;
    while (hkeys[i]) i = (i + 1) & HASH_MASK;
    hkeys[i] = key;
    hvals[i] = val;
}

static inline uint32_t hash_get(uint32_t key)
{
    uint32_t i = (key * 0x9E3779B1u) >> 18;
    while (hkeys[i] != key) i = (i + 1) & HASH_MASK;
    return hvals[i];
}

static uint32_t binom_t[53][6];

void plo5_init(void)
{
    if (initialized) return;

    for (int n = 0; n < 53; n++) {
        binom_t[n][0] = 1;
        for (int k = 1; k < 6; k++)
            binom_t[n][k] = (n == 0) ? 0 : binom_t[n - 1][k - 1] + binom_t[n - 1][k];
    }

    for (int r = 0; r < 13; r++)
        for (int s = 0; s < 4; s++)
            card_key[r * 4 + s] =
                (1u << (16 + r)) | (1u << (12 + s)) | rank_prime[r];

    /* 5 distinct ranks: straight / high card, and the flush variants */
    for (int a = 0; a < 13; a++)
    for (int b = a + 1; b < 13; b++)
    for (int c = b + 1; c < 13; c++)
    for (int d = c + 1; d < 13; d++)
    for (int e = d + 1; e < 13; e++) {
        uint32_t m = (1u << a) | (1u << b) | (1u << c) | (1u << d) | (1u << e);
        int hi = -1;
        if (m == 0x100Fu)          hi = 3;  /* wheel: A2345, 5-high */
        else if (e - a == 4)       hi = e;  /* 5 consecutive ranks  */
        uniq5_tbl[m] = (hi >= 0) ? (((uint32_t)CAT_STRAIGHT << 24) | (uint32_t)hi)
                                 : (((uint32_t)CAT_HIGH     << 24) | m);
        flush_tbl[m] = (hi >= 0) ? (((uint32_t)CAT_STFLUSH  << 24) | (uint32_t)hi)
                                 : (((uint32_t)CAT_FLUSH    << 24) | m);
    }

    /* rank multisets with at least one repeat -> prime-product hash */
    for (int a = 0; a < 13; a++)
    for (int b = a; b < 13; b++)
    for (int c = b; c < 13; c++)
    for (int d = c; d < 13; d++)
    for (int e = d; e < 13; e++) {
        int cnt[13] = {0};
        cnt[a]++; cnt[b]++; cnt[c]++; cnt[d]++; cnt[e]++;
        int maxc = 0, over = 0, distinct = 0;
        for (int r = 0; r < 13; r++) {
            if (cnt[r] > 4) over = 1;
            if (cnt[r] > maxc) maxc = cnt[r];
            if (cnt[r]) distinct++;
        }
        if (over || maxc == 1) continue;    /* impossible, or handled above */

        int quad = -1, trip = -1, phi = -1, plo = -1;
        uint32_t kick = 0;                  /* mask of singleton ranks */
        for (int r = 12; r >= 0; r--) {
            if (cnt[r] == 4) quad = r;
            else if (cnt[r] == 3) trip = r;
            else if (cnt[r] == 2) { if (phi < 0) phi = r; else plo = r; }
            else if (cnt[r] == 1) kick |= 1u << r;
        }

        uint32_t val;
        if (quad >= 0) {
            int k = 0; while (!(kick >> k & 1)) k++;
            val = ((uint32_t)CAT_QUADS << 24) | ((uint32_t)quad << 4) | (uint32_t)k;
        } else if (trip >= 0 && phi >= 0) {
            val = ((uint32_t)CAT_FULL << 24) | ((uint32_t)trip << 4) | (uint32_t)phi;
        } else if (trip >= 0) {
            val = ((uint32_t)CAT_TRIPS << 24) | ((uint32_t)trip << 13) | kick;
        } else if (plo >= 0) {
            int k = 0; while (!(kick >> k & 1)) k++;
            val = ((uint32_t)CAT_TWOPAIR << 24) |
                  ((uint32_t)phi << 8) | ((uint32_t)plo << 4) | (uint32_t)k;
        } else {
            val = ((uint32_t)CAT_PAIR << 24) | ((uint32_t)phi << 13) | kick;
        }

        uint32_t key = rank_prime[a] * rank_prime[b] * rank_prime[c] *
                       rank_prime[d] * rank_prime[e];
        hash_put(key, val);
    }

    initialized = 1;
}

uint32_t plo5_eval5(const int c[5])
{
    uint32_t k0 = card_key[c[0]], k1 = card_key[c[1]], k2 = card_key[c[2]],
             k3 = card_key[c[3]], k4 = card_key[c[4]];
    uint32_t orv  = k0 | k1 | k2 | k3 | k4;
    uint32_t andv = k0 & k1 & k2 & k3 & k4;
    uint32_t q = orv >> 16;
    if (andv & 0xF000u) return flush_tbl[q];
    uint32_t u = uniq5_tbl[q];
    if (u) return u;
    return hash_get((k0 & 0xFFu) * (k1 & 0xFFu) * (k2 & 0xFFu) *
                    (k3 & 0xFFu) * (k4 & 0xFFu));
}

/* ------------------------------------------------------------------ */
/* Omaha combination machinery                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t a, o, p; } combo_t;   /* AND, OR, prime product */

static const uint8_t PAIR_IX[10][2] = {
    {0,1},{0,2},{0,3},{0,4},{1,2},{1,3},{1,4},{2,3},{2,4},{3,4}
};
static const uint8_t TRIP_IX[10][3] = {
    {0,1,2},{0,1,3},{0,1,4},{0,2,3},{0,2,4},
    {0,3,4},{1,2,3},{1,2,4},{1,3,4},{2,3,4}
};

static void make_pairs(const uint32_t k[5], combo_t out[10])
{
    for (int i = 0; i < 10; i++) {
        uint32_t x = k[PAIR_IX[i][0]], y = k[PAIR_IX[i][1]];
        out[i].a = x & y;
        out[i].o = x | y;
        out[i].p = (x & 0xFFu) * (y & 0xFFu);
    }
}

static void make_trips(const uint32_t k[5], combo_t out[10])
{
    for (int i = 0; i < 10; i++) {
        uint32_t x = k[TRIP_IX[i][0]], y = k[TRIP_IX[i][1]], z = k[TRIP_IX[i][2]];
        out[i].a = x & y & z;
        out[i].o = x | y | z;
        out[i].p = (x & 0xFFu) * (y & 0xFFu) * (z & 0xFFu);
    }
}

/* best of the 100 (2 from hand, 3 from board) five-card hands */
static inline uint32_t best_val(const combo_t hp[10], const combo_t bt[10])
{
    uint32_t best = 0;
    for (int j = 0; j < 10; j++) {
        uint32_t ba = bt[j].a, bo = bt[j].o, bp = bt[j].p;
        for (int i = 0; i < 10; i++) {
            uint32_t q = (hp[i].o | bo) >> 16, v;
            if (hp[i].a & ba & 0xF000u)      v = flush_tbl[q];
            else if ((v = uniq5_tbl[q]) == 0) v = hash_get(hp[i].p * bp);
            if (v > best) best = v;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* PRNG: xoshiro256** seeded via splitmix64                            */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t s[4]; } rng_t;

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void rng_seed(rng_t *r, uint64_t seed)
{
    for (int i = 0; i < 4; i++) r->s[i] = splitmix64(&seed);
}

static inline uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t rng_next(rng_t *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

/* uniform in [0, n) using the high 32 bits (Lemire multiply-shift) */
static inline uint32_t rng_below(rng_t *r, uint32_t n)
{
    return (uint32_t)(((rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

/* ------------------------------------------------------------------ */
/* Hand-strength percentile ranks                                      */
/*                                                                     */
/* File layout ("plo5rank.bin"):                                       */
/*   8  bytes magic "PLO5RNK1"                                         */
/*   u32 combo count (2,598,960), u32 trials per class                 */
/*   NCOMBO x 5 bytes  card ids of each combo, weakest first           */
/*   NCOMBO x u32      rank position indexed by colex index            */
/* ------------------------------------------------------------------ */

#define NCOMBO 2598960u
#define RANK_MAGIC "PLO5RNK1"

static uint8_t  (*g_rk_cards)[5] = NULL;  /* [NCOMBO][5], weakest first */
static uint32_t *g_rk_of_colex   = NULL;  /* [NCOMBO]                   */

static void sort5(int *c)
{
    for (int i = 1; i < 5; i++) {
        int v = c[i], j = i - 1;
        while (j >= 0 && c[j] > v) { c[j + 1] = c[j]; j--; }
        c[j + 1] = v;
    }
}

/* colex index of a strictly ascending 5-card set */
static uint32_t colex5(const int *c)
{
    return binom_t[c[0]][1] + binom_t[c[1]][2] + binom_t[c[2]][3] +
           binom_t[c[3]][4] + binom_t[c[4]][5];
}

int plo5_ranks_loaded(void) { return g_rk_cards != NULL; }

int plo5_ranks_load(const char *path)
{
    if (!initialized) plo5_init();
    FILE *f = fopen(path, "rb");
    if (!f) return PLO5_ERR_IO;
    char magic[8];
    uint32_t n = 0, tpc = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, RANK_MAGIC, 8) != 0 ||
        fread(&n, 4, 1, f) != 1 || fread(&tpc, 4, 1, f) != 1 || n != NCOMBO) {
        fclose(f);
        return PLO5_ERR_IO;
    }
    uint8_t  (*rc)[5] = malloc((size_t)NCOMBO * 5);
    uint32_t *rr      = malloc((size_t)NCOMBO * 4);
    if (!rc || !rr) { free(rc); free(rr); fclose(f); return PLO5_ERR_ARG; }
    if (fread(rc, 5, NCOMBO, f) != NCOMBO ||
        fread(rr, 4, NCOMBO, f) != NCOMBO) {
        free(rc); free(rr); fclose(f);
        return PLO5_ERR_IO;
    }
    fclose(f);
    free(g_rk_cards); free(g_rk_of_colex);
    g_rk_cards = rc;
    g_rk_of_colex = rr;
    return PLO5_OK;
}

double plo5_hand_percentile(const int cards[5])
{
    if (!g_rk_cards) return -1.0;
    int c[5];
    memcpy(c, cards, sizeof c);
    for (int i = 0; i < 5; i++) if (c[i] < 0 || c[i] > 51) return -1.0;
    sort5(c);
    uint32_t r = g_rk_of_colex[colex5(c)];
    return (r + 0.5) * 100.0 / (double)NCOMBO;
}

int plo5_percentile_hand(double pct, int out[5])
{
    if (!g_rk_cards) return PLO5_ERR_RANKS;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t i = (uint32_t)(pct / 100.0 * (double)NCOMBO);
    if (i >= NCOMBO) i = NCOMBO - 1;
    for (int k = 0; k < 5; k++) out[k] = g_rk_cards[i][k];
    return PLO5_OK;
}

/* ---- generation ---- */

#ifdef _WIN32
static long atomic_inc(volatile long *p) { return InterlockedIncrement(p); }
#else
static long atomic_inc(volatile long *p) { return __sync_add_and_fetch(p, 1); }
#endif

/* heads-up equity of hand vs one random hand, random board */
static double gen_eq_vs_random(const uint8_t hand[5], uint32_t trials, rng_t *rng)
{
    uint32_t hk[5];
    combo_t hp[10], op[10], bt[10];
    uint64_t used = 0;
    for (int i = 0; i < 5; i++) {
        hk[i] = card_key[hand[i]];
        used |= 1ull << hand[i];
    }
    make_pairs(hk, hp);
    int deck[47], nd = 0;
    for (int c = 0; c < 52; c++)
        if (!(used >> c & 1)) deck[nd++] = c;

    double sum = 0;
    for (uint32_t t = 0; t < trials; t++) {
        for (int i = 0; i < 10; i++) {
            int j = i + (int)rng_below(rng, (uint32_t)(47 - i));
            int tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
        }
        uint32_t ok[5], bk[5];
        for (int i = 0; i < 5; i++) { ok[i] = card_key[deck[i]]; bk[i] = card_key[deck[5 + i]]; }
        make_pairs(ok, op);
        make_trips(bk, bt);
        uint32_t a = best_val(hp, bt), b = best_val(op, bt);
        sum += a > b ? 1.0 : (a == b ? 0.5 : 0.0);
    }
    return sum / (double)trials;
}

typedef struct {
    int i0, i1;
    uint64_t seed;
    uint32_t trials;
    const uint8_t (*rep)[5];
    double *eq;
    volatile long *done;
} genw_t;

#ifdef _WIN32
static DWORD WINAPI gen_thread(LPVOID vp)
#else
static void *gen_thread(void *vp)
#endif
{
    genw_t *w = (genw_t *)vp;
    rng_t rng;
    rng_seed(&rng, w->seed);
    for (int i = w->i0; i < w->i1; i++) {
        w->eq[i] = gen_eq_vs_random(w->rep[i], w->trials, &rng);
        atomic_inc(w->done);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

typedef struct { double eq; uint32_t id; } clsord_t;

static int clsord_cmp(const void *a, const void *b)
{
    const clsord_t *x = a, *y = b;
    if (x->eq < y->eq) return -1;
    if (x->eq > y->eq) return 1;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

#define CLS_CAP   262144
#define CHASH_BITS 19
#define CHASH_SIZE (1u << CHASH_BITS)

int plo5_ranks_generate(const char *path, uint32_t trials_per_class,
                        int nthreads, uint64_t seed,
                        void (*progress)(int done, int total, void *ud),
                        void *ud)
{
    if (!initialized) plo5_init();
    if (trials_per_class < 1000) trials_per_class = 1000;

    uint32_t *cls_of = malloc((size_t)NCOMBO * 4);
    uint64_t *chk    = calloc(CHASH_SIZE, 8);
    uint32_t *chv    = malloc((size_t)CHASH_SIZE * 4);
    uint8_t (*rep)[5] = malloc((size_t)CLS_CAP * 5);
    double  *eq      = malloc((size_t)CLS_CAP * 8);
    if (!cls_of || !chk || !chv || !rep || !eq) goto oom;

    /* the 24 suit permutations */
    int perm[24][4], npm = 0;
    for (int a = 0; a < 4; a++)
    for (int b = 0; b < 4; b++) {
        if (b == a) continue;
        for (int c = 0; c < 4; c++) {
            if (c == a || c == b) continue;
            int d = 0 + 1 + 2 + 3 - a - b - c;
            perm[npm][0] = a; perm[npm][1] = b;
            perm[npm][2] = c; perm[npm][3] = d;
            npm++;
        }
    }

    /* pass 1: canonicalize every combo into suit-isomorphism classes */
    int nc = 0;
    {
        int c[5];
        for (c[0] = 0; c[0] < 48; c[0]++)
        for (c[1] = c[0] + 1; c[1] < 49; c[1]++)
        for (c[2] = c[1] + 1; c[2] < 50; c[2]++)
        for (c[3] = c[2] + 1; c[3] < 51; c[3]++)
        for (c[4] = c[3] + 1; c[4] < 52; c[4]++) {
            uint64_t best = ~0ull;
            for (int p = 0; p < 24; p++) {
                int m[5];
                for (int i = 0; i < 5; i++)
                    m[i] = (c[i] & ~3) | perm[p][c[i] & 3];
                sort5(m);
                uint64_t key = (uint64_t)m[0] | (uint64_t)m[1] << 6 |
                               (uint64_t)m[2] << 12 | (uint64_t)m[3] << 18 |
                               (uint64_t)m[4] << 24;
                if (key < best) best = key;
            }
            uint32_t h = (uint32_t)((best * 0x9E3779B97F4A7C15ull) >> (64 - CHASH_BITS));
            while (chk[h] && chk[h] != best) h = (h + 1) & (CHASH_SIZE - 1);
            if (!chk[h]) {
                if (nc >= CLS_CAP) goto oom;
                chk[h] = best;
                chv[h] = (uint32_t)nc;
                for (int i = 0; i < 5; i++) rep[nc][i] = (uint8_t)c[i];
                nc++;
            }
            cls_of[colex5(c)] = chv[h];
        }
    }

    /* pass 2: MC equity vs random per class, threaded */
    {
        int nt = nthreads < 1 ? 1 : (nthreads > 64 ? 64 : nthreads);
        if (nt > nc) nt = nc;
        volatile long done = 0;
        genw_t w[64];
        int per = nc / nt, remn = nc % nt, at = 0;
        for (int i = 0; i < nt; i++) {
            w[i].i0 = at;
            at += per + (i < remn ? 1 : 0);
            w[i].i1 = at;
            uint64_t s = seed + 0x1234u + (uint64_t)i;
            w[i].seed = splitmix64(&s);
            w[i].trials = trials_per_class;
            w[i].rep = (const uint8_t (*)[5])rep;
            w[i].eq = eq;
            w[i].done = &done;
        }
#ifdef _WIN32
        HANDLE th[64];
        for (int i = 0; i < nt; i++)
            th[i] = CreateThread(NULL, 0, gen_thread, &w[i], 0, NULL);
        while (done < nc) {
            Sleep(200);
            if (progress) progress((int)done, nc, ud);
        }
        for (int i = 0; i < nt; i++)
            if (th[i]) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
#else
        pthread_t th[64];
        for (int i = 0; i < nt; i++)
            pthread_create(&th[i], NULL, gen_thread, &w[i]);
        while (done < nc) {
            struct timespec ts = { 0, 200000000 };
            nanosleep(&ts, NULL);
            if (progress) progress((int)done, nc, ud);
        }
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
#endif
    }

    /* pass 3: order classes by equity, lay out combos weakest first */
    {
        clsord_t *ord = malloc((size_t)nc * sizeof *ord);
        uint32_t *off = malloc((size_t)nc * 4);
        uint32_t *csize = calloc((size_t)nc, 4);
        uint8_t (*rc)[5] = malloc((size_t)NCOMBO * 5);
        uint32_t *rr = malloc((size_t)NCOMBO * 4);
        if (!ord || !off || !csize || !rc || !rr) {
            free(ord); free(off); free(csize); free(rc); free(rr);
            goto oom;
        }
        for (uint32_t i = 0; i < NCOMBO; i++) csize[cls_of[i]]++;
        for (int i = 0; i < nc; i++) { ord[i].eq = eq[i]; ord[i].id = (uint32_t)i; }
        qsort(ord, (size_t)nc, sizeof *ord, clsord_cmp);
        uint32_t cum = 0;
        for (int k = 0; k < nc; k++) {
            off[ord[k].id] = cum;
            cum += csize[ord[k].id];
        }
        int c[5];
        for (c[0] = 0; c[0] < 48; c[0]++)
        for (c[1] = c[0] + 1; c[1] < 49; c[1]++)
        for (c[2] = c[1] + 1; c[2] < 50; c[2]++)
        for (c[3] = c[2] + 1; c[3] < 51; c[3]++)
        for (c[4] = c[3] + 1; c[4] < 52; c[4]++) {
            uint32_t cx = colex5(c);
            uint32_t pos = off[cls_of[cx]]++;
            for (int i = 0; i < 5; i++) rc[pos][i] = (uint8_t)c[i];
            rr[cx] = pos;
        }
        free(ord); free(off); free(csize);
        free(g_rk_cards); free(g_rk_of_colex);
        g_rk_cards = rc;
        g_rk_of_colex = rr;
    }

    free(cls_of); free(chk); free(chv); free(rep); free(eq);
    if (progress) progress(nc, nc, ud);

    /* write to disk (table stays loaded even if this fails) */
    {
        FILE *f = fopen(path, "wb");
        if (!f) return PLO5_ERR_IO;
        uint32_t n = NCOMBO;
        int ok = fwrite(RANK_MAGIC, 1, 8, f) == 8 &&
                 fwrite(&n, 4, 1, f) == 1 &&
                 fwrite(&trials_per_class, 4, 1, f) == 1 &&
                 fwrite(g_rk_cards, 5, NCOMBO, f) == NCOMBO &&
                 fwrite(g_rk_of_colex, 4, NCOMBO, f) == NCOMBO;
        fclose(f);
        if (!ok) return PLO5_ERR_IO;
    }
    return PLO5_OK;

oom:
    free(cls_of); free(chk); free(chv); free(rep); free(eq);
    return PLO5_ERR_ARG;
}

/* ------------------------------------------------------------------ */
/* Board-conditional rankings                                          */
/*                                                                     */
/* Given a flop/turn/river, rank every possible 5-card holding:        */
/*   river: made-hand strength                                         */
/*   turn : mean showdown percentile over all rivers (exact)           */
/*   flop : mean showdown percentile over sampled runouts              */
/* ------------------------------------------------------------------ */

static int take_card(int c, uint64_t *used);
static uint64_t choose64(int n, int k);

/* all 7462 distinct 5-card hand values, sorted, for ordinal lookups */
static uint32_t g_vals[7680];
static int      g_nvals = 0;

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void build_vals(void)
{
    if (g_nvals) return;
    int n = 0;
    for (int i = 0; i < 8192; i++) {
        if (uniq5_tbl[i]) g_vals[n++] = uniq5_tbl[i];
        if (flush_tbl[i]) g_vals[n++] = flush_tbl[i];
    }
    for (int i = 0; i < HASH_SIZE; i++)
        if (hkeys[i]) g_vals[n++] = hvals[i];
    qsort(g_vals, (size_t)n, 4, u32_cmp);
    /* dedup (values are distinct by construction, but be safe) */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (i == 0 || g_vals[i] != g_vals[i - 1]) g_vals[m++] = g_vals[i];
    g_nvals = m;
}

static inline uint16_t val_ord(uint32_t v)
{
    int lo = 0, hi = g_nvals - 1;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (g_vals[mid] < v) lo = mid + 1; else hi = mid;
    }
    return (uint16_t)lo;
}

/* generic parallel-for over [0, n) */
typedef struct { void (*fn)(int, int, void *); void *ctx; int i0, i1; } pfj_t;

#ifdef _WIN32
static DWORD WINAPI pf_thread(LPVOID vp)
{
    pfj_t *j = (pfj_t *)vp;
    j->fn(j->i0, j->i1, j->ctx);
    return 0;
}
#else
static void *pf_thread(void *vp)
{
    pfj_t *j = (pfj_t *)vp;
    j->fn(j->i0, j->i1, j->ctx);
    return NULL;
}
#endif

static void parallel_for(int n, int nt, void (*fn)(int, int, void *), void *ctx)
{
    if (nt < 1) nt = 1;
    if (nt > 64) nt = 64;
    if (nt > n) nt = n > 0 ? n : 1;
    if (nt == 1) { fn(0, n, ctx); return; }
    pfj_t j[64];
    int per = n / nt, rem = n % nt, at = 0;
    for (int i = 0; i < nt; i++) {
        j[i].fn = fn; j[i].ctx = ctx;
        j[i].i0 = at;
        at += per + (i < rem ? 1 : 0);
        j[i].i1 = at;
    }
#ifdef _WIN32
    HANDLE th[64];
    for (int i = 0; i < nt; i++)
        th[i] = CreateThread(NULL, 0, pf_thread, &j[i], 0, NULL);
    for (int i = 0; i < nt; i++)
        if (th[i]) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
        else pf_thread(&j[i]);
#else
    pthread_t th[64];
    for (int i = 0; i < nt; i++)
        if (pthread_create(&th[i], NULL, pf_thread, &j[i]) != 0)
            pf_thread(&j[i]);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
#endif
}

/* the one cached board table */
static int      g_bt_nboard = 0;          /* 0 = none */
static int      g_bt_board[5];            /* sorted */
static uint32_t g_bt_n = 0;
static uint8_t (*g_bt_cards)[5] = NULL;   /* [g_bt_n][5], weakest first */
static uint32_t *g_bt_of_colex  = NULL;   /* [NCOMBO], ~0u = invalid */

static int board_match(const int *board, int nboard)
{
    if (nboard == 0 || g_bt_nboard != nboard) return 0;
    int b[5];
    memcpy(b, board, (size_t)nboard * sizeof(int));
    for (int i = 1; i < nboard; i++) {          /* tiny insertion sort */
        int v = b[i], j = i - 1;
        while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; }
        b[j + 1] = v;
    }
    return memcmp(b, g_bt_board, (size_t)nboard * sizeof(int)) == 0;
}

typedef struct {
    const uint8_t (*hold)[5];
    uint32_t nh;
    combo_t  bt10[10];
    uint64_t excl;              /* runout cards */
    uint16_t *ord;
    float    *sum;
    uint16_t *cnt;
    uint32_t *below, *hist;
    double    scale;            /* 100 / valid_total */
} bctx_t;

static void bphase_vals(int i0, int i1, void *vp)
{
    bctx_t *c = (bctx_t *)vp;
    for (int h = i0; h < i1; h++) {
        const uint8_t *cd = c->hold[h];
        uint64_t m = (1ull << cd[0]) | (1ull << cd[1]) | (1ull << cd[2]) |
                     (1ull << cd[3]) | (1ull << cd[4]);
        if (m & c->excl) { c->ord[h] = 0xFFFF; continue; }
        uint32_t hk[5];
        combo_t hp[10];
        for (int i = 0; i < 5; i++) hk[i] = card_key[cd[i]];
        make_pairs(hk, hp);
        c->ord[h] = val_ord(best_val(hp, c->bt10));
    }
}

static void bphase_acc(int i0, int i1, void *vp)
{
    bctx_t *c = (bctx_t *)vp;
    for (int h = i0; h < i1; h++) {
        uint16_t o = c->ord[h];
        if (o == 0xFFFF) continue;
        double pct = ((double)c->below[o] + 0.5 * (double)c->hist[o]) * c->scale;
        c->sum[h] += (float)pct;
        c->cnt[h]++;
    }
}

typedef struct { float sc; uint32_t h; } bscore_t;

static int bscore_cmp(const void *a, const void *b)
{
    const bscore_t *x = a, *y = b;
    if (x->sc < y->sc) return -1;
    if (x->sc > y->sc) return 1;
    return x->h < y->h ? -1 : x->h > y->h ? 1 : 0;
}

/* core builder: ranks all holdings for one board, writes fresh arrays to
 * the out params (no global state, no caching) */
static int bt_build_core(const int *board, int nboard, int flop_runouts,
                         int nthreads,
                         void (*progress)(int done, int total, void *ud),
                         void *ud,
                         uint8_t (**out_cards)[5], uint32_t **out_colex,
                         uint32_t *out_n)
{
    uint64_t used = 0;
    for (int i = 0; i < nboard; i++) {
        int rc = take_card(board[i], &used);
        if (rc != PLO5_OK) return rc;
    }
    build_vals();
    if (flop_runouts <= 0) flop_runouts = 100;

    int deck[52], nd = 0;
    for (int c = 0; c < 52; c++)
        if (!(used >> c & 1)) deck[nd++] = c;

    uint32_t nh = (uint32_t)choose64(nd, 5);
    uint8_t (*hold)[5] = malloc((size_t)nh * 5);
    float    *sum  = calloc(nh, 4);
    uint16_t *cnt  = calloc(nh, 2);
    uint16_t *ord  = malloc((size_t)nh * 2);
    uint32_t *hist = malloc((size_t)g_nvals * 4);
    uint32_t *below= malloc((size_t)g_nvals * 4);
    bscore_t *bs   = malloc((size_t)nh * sizeof(bscore_t));
    if (!hold || !sum || !cnt || !ord || !hist || !below || !bs) {
        free(hold); free(sum); free(cnt); free(ord);
        free(hist); free(below); free(bs);
        return PLO5_ERR_ARG;
    }
    {
        uint32_t k = 0;
        for (int a = 0; a < nd - 4; a++)
        for (int b = a + 1; b < nd - 3; b++)
        for (int c = b + 1; c < nd - 2; c++)
        for (int d = c + 1; d < nd - 1; d++)
        for (int e = d + 1; e < nd; e++) {
            hold[k][0] = (uint8_t)deck[a]; hold[k][1] = (uint8_t)deck[b];
            hold[k][2] = (uint8_t)deck[c]; hold[k][3] = (uint8_t)deck[d];
            hold[k][4] = (uint8_t)deck[e];
            k++;
        }
    }

    /* runout list: pairs of extra board cards (flop), single river (turn),
     * or none (river) */
    int runA[1332], runB[1332], nrun;
    if (nboard == 5) {
        nrun = 1; runA[0] = -1; runB[0] = -1;
    } else if (nboard == 4) {
        nrun = nd;
        for (int i = 0; i < nd; i++) { runA[i] = deck[i]; runB[i] = -1; }
    } else {
        int np2 = 0;
        static int pa[1332], pb[1332];
        for (int i = 0; i < nd; i++)
            for (int j = i + 1; j < nd; j++) { pa[np2] = deck[i]; pb[np2] = deck[j]; np2++; }
        rng_t rng;
        rng_seed(&rng, 0xB0A2D5EEDull);
        for (int i = 0; i < np2 - 1; i++) {       /* Fisher-Yates */
            int j = i + (int)rng_below(&rng, (uint32_t)(np2 - i));
            int t = pa[i]; pa[i] = pa[j]; pa[j] = t;
            t = pb[i]; pb[i] = pb[j]; pb[j] = t;
        }
        nrun = flop_runouts < np2 ? flop_runouts : np2;
        for (int i = 0; i < nrun; i++) { runA[i] = pa[i]; runB[i] = pb[i]; }
    }

    bctx_t ctx;
    ctx.hold = (const uint8_t (*)[5])hold;
    ctx.nh = nh;
    ctx.ord = ord; ctx.sum = sum; ctx.cnt = cnt;
    ctx.below = below; ctx.hist = hist;

    for (int r = 0; r < nrun; r++) {
        uint32_t bkey[5];
        for (int i = 0; i < nboard; i++) bkey[i] = card_key[board[i]];
        int nb2 = nboard;
        ctx.excl = 0;
        if (runA[r] >= 0) { bkey[nb2++] = card_key[runA[r]]; ctx.excl |= 1ull << runA[r]; }
        if (runB[r] >= 0) { bkey[nb2++] = card_key[runB[r]]; ctx.excl |= 1ull << runB[r]; }
        make_trips(bkey, ctx.bt10);

        parallel_for((int)nh, nthreads, bphase_vals, &ctx);

        memset(hist, 0, (size_t)g_nvals * 4);
        uint64_t total = 0;
        for (uint32_t h = 0; h < nh; h++)
            if (ord[h] != 0xFFFF) { hist[ord[h]]++; total++; }
        uint32_t cum = 0;
        for (int i = 0; i < g_nvals; i++) { below[i] = cum; cum += hist[i]; }
        ctx.scale = total ? 100.0 / (double)total : 0.0;

        parallel_for((int)nh, nthreads, bphase_acc, &ctx);
        if (progress) progress(r + 1, nrun, ud);
    }

    for (uint32_t h = 0; h < nh; h++) {
        bs[h].sc = cnt[h] ? sum[h] / (float)cnt[h] : 50.0f;
        bs[h].h = h;
    }
    qsort(bs, nh, sizeof(bscore_t), bscore_cmp);

    uint8_t (*btc)[5] = malloc((size_t)nh * 5);
    uint32_t *btx = malloc((size_t)NCOMBO * 4);
    if (!btc || !btx) {
        free(btc); free(btx);
        free(hold); free(sum); free(cnt); free(ord);
        free(hist); free(below); free(bs);
        return PLO5_ERR_ARG;
    }
    memset(btx, 0xFF, (size_t)NCOMBO * 4);
    for (uint32_t pos = 0; pos < nh; pos++) {
        const uint8_t *cd = hold[bs[pos].h];
        int c5[5];
        for (int i = 0; i < 5; i++) {
            btc[pos][i] = cd[i];
            c5[i] = cd[i];
        }
        btx[colex5(c5)] = pos;      /* holdings are stored ascending */
    }

    free(hold); free(sum); free(cnt); free(ord);
    free(hist); free(below); free(bs);
    *out_cards = btc;
    *out_colex = btx;
    *out_n = nh;
    return PLO5_OK;
}

static void sort_board(int *b, int n)
{
    for (int i = 1; i < n; i++) {
        int v = b[i], j = i - 1;
        while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; }
        b[j + 1] = v;
    }
}

int plo5_board_ranks_build(const int *board, int nboard, int flop_runouts,
                           int nthreads,
                           void (*progress)(int done, int total, void *ud),
                           void *ud)
{
    if (!initialized) plo5_init();
    if (!board || (nboard != 3 && nboard != 4 && nboard != 5))
        return PLO5_ERR_BOARD;
    uint64_t used = 0;
    for (int i = 0; i < nboard; i++) {
        int rc = take_card(board[i], &used);
        if (rc != PLO5_OK) return rc;
    }
    if (board_match(board, nboard)) return PLO5_OK;    /* cached */

    uint8_t (*c)[5];
    uint32_t *x, n;
    int rc = bt_build_core(board, nboard, flop_runouts, nthreads,
                           progress, ud, &c, &x, &n);
    if (rc != PLO5_OK) return rc;
    free(g_bt_cards); free(g_bt_of_colex);
    g_bt_cards = c;
    g_bt_of_colex = x;
    g_bt_n = n;
    g_bt_nboard = nboard;
    memcpy(g_bt_board, board, (size_t)nboard * sizeof(int));
    sort_board(g_bt_board, nboard);
    return PLO5_OK;
}

/* ---- combined two-board distribution -------------------------------- */

static int      g_bt2_nb1 = 0, g_bt2_nb2 = 0;
static int      g_bt2_b1[5], g_bt2_b2[5];         /* each sorted */
static uint32_t g_bt2_n = 0;
static uint8_t (*g_bt2_cards)[5] = NULL;
static uint32_t *g_bt2_of_colex  = NULL;

static int bt2_match(const int *b1, int nb1, const int *b2, int nb2)
{
    if (!g_bt2_nb1) return 0;
    int s1[5], s2[5];
    memcpy(s1, b1, (size_t)nb1 * sizeof(int));
    memcpy(s2, b2, (size_t)nb2 * sizeof(int));
    sort_board(s1, nb1);
    sort_board(s2, nb2);
    if (nb1 == g_bt2_nb1 && nb2 == g_bt2_nb2 &&
        !memcmp(s1, g_bt2_b1, (size_t)nb1 * sizeof(int)) &&
        !memcmp(s2, g_bt2_b2, (size_t)nb2 * sizeof(int))) return 1;
    if (nb1 == g_bt2_nb2 && nb2 == g_bt2_nb1 &&
        !memcmp(s1, g_bt2_b2, (size_t)nb1 * sizeof(int)) &&
        !memcmp(s2, g_bt2_b1, (size_t)nb2 * sizeof(int))) return 1;
    return 0;
}

int plo5_board_ranks_build2(const int *b1, int nb1, const int *b2, int nb2,
                            int flop_runouts, int nthreads,
                            void (*progress)(int done, int total, void *ud),
                            void *ud)
{
    if (!initialized) plo5_init();
    if (!b1 || !b2 ||
        (nb1 != 3 && nb1 != 4 && nb1 != 5) ||
        (nb2 != 3 && nb2 != 4 && nb2 != 5)) return PLO5_ERR_BOARD;
    uint64_t used = 0;
    for (int i = 0; i < nb1; i++) {
        int rc = take_card(b1[i], &used);
        if (rc != PLO5_OK) return rc;
    }
    for (int i = 0; i < nb2; i++) {
        int rc = take_card(b2[i], &used);
        if (rc != PLO5_OK) return rc;
    }
    if (bt2_match(b1, nb1, b2, nb2)) return PLO5_OK;

    uint8_t (*cA)[5], (*cB)[5];
    uint32_t *xA, *xB, nA, nB;
    int rc = bt_build_core(b1, nb1, flop_runouts, nthreads, progress, ud,
                           &cA, &xA, &nA);
    if (rc != PLO5_OK) return rc;
    free(cA);
    rc = bt_build_core(b2, nb2, flop_runouts, nthreads, progress, ud,
                       &cB, &xB, &nB);
    if (rc != PLO5_OK) { free(xA); return rc; }
    free(cB);

    int deck[52], nd = 0;
    for (int c = 0; c < 52; c++)
        if (!(used >> c & 1)) deck[nd++] = c;
    uint32_t nh = (uint32_t)choose64(nd, 5);

    uint8_t (*hold)[5] = malloc((size_t)nh * 5);
    bscore_t *bs = malloc((size_t)nh * sizeof(bscore_t));
    uint8_t (*btc)[5] = malloc((size_t)nh * 5);
    uint32_t *btx = malloc((size_t)NCOMBO * 4);
    if (!hold || !bs || !btc || !btx) {
        free(hold); free(bs); free(btc); free(btx);
        free(xA); free(xB);
        return PLO5_ERR_ARG;
    }
    {
        uint32_t k = 0;
        int c5[5];
        for (int a = 0; a < nd - 4; a++)
        for (int b = a + 1; b < nd - 3; b++)
        for (int c = b + 1; c < nd - 2; c++)
        for (int d = c + 1; d < nd - 1; d++)
        for (int e = d + 1; e < nd; e++) {
            c5[0] = deck[a]; c5[1] = deck[b]; c5[2] = deck[c];
            c5[3] = deck[d]; c5[4] = deck[e];
            uint32_t cx = colex5(c5);
            double pA = ((double)xA[cx] + 0.5) / (double)nA;
            double pB = ((double)xB[cx] + 0.5) / (double)nB;
            for (int i = 0; i < 5; i++) hold[k][i] = (uint8_t)c5[i];
            bs[k].sc = (float)(pA + pB);
            bs[k].h = k;
            k++;
        }
    }
    free(xA); free(xB);
    qsort(bs, nh, sizeof(bscore_t), bscore_cmp);
    memset(btx, 0xFF, (size_t)NCOMBO * 4);
    for (uint32_t pos = 0; pos < nh; pos++) {
        const uint8_t *cd = hold[bs[pos].h];
        int c5[5];
        for (int i = 0; i < 5; i++) { btc[pos][i] = cd[i]; c5[i] = cd[i]; }
        btx[colex5(c5)] = pos;
    }
    free(hold); free(bs);

    free(g_bt2_cards); free(g_bt2_of_colex);
    g_bt2_cards = btc;
    g_bt2_of_colex = btx;
    g_bt2_n = nh;
    g_bt2_nb1 = nb1;
    g_bt2_nb2 = nb2;
    memcpy(g_bt2_b1, b1, (size_t)nb1 * sizeof(int));
    memcpy(g_bt2_b2, b2, (size_t)nb2 * sizeof(int));
    sort_board(g_bt2_b1, nb1);
    sort_board(g_bt2_b2, nb2);
    return PLO5_OK;
}

int plo5_board_ranks_state2(int b1_out[5], int *nb1_out,
                            int b2_out[5], int *nb2_out)
{
    if (!g_bt2_nb1) return 0;
    if (b1_out) memcpy(b1_out, g_bt2_b1, (size_t)g_bt2_nb1 * sizeof(int));
    if (nb1_out) *nb1_out = g_bt2_nb1;
    if (b2_out) memcpy(b2_out, g_bt2_b2, (size_t)g_bt2_nb2 * sizeof(int));
    if (nb2_out) *nb2_out = g_bt2_nb2;
    return 1;
}

double plo5_hand_percentile_2b(const int cards[5], const int *b1, int nb1,
                               const int *b2, int nb2)
{
    if (!bt2_match(b1, nb1, b2, nb2)) return -1.0;
    int c[5];
    memcpy(c, cards, sizeof c);
    for (int i = 0; i < 5; i++) if (c[i] < 0 || c[i] > 51) return -1.0;
    sort5(c);
    uint32_t r = g_bt2_of_colex[colex5(c)];
    if (r == 0xFFFFFFFFu) return -1.0;
    return (r + 0.5) * 100.0 / (double)g_bt2_n;
}

int plo5_percentile_hand_2b(double pct, const int *b1, int nb1,
                            const int *b2, int nb2, int out[5])
{
    if (!bt2_match(b1, nb1, b2, nb2)) return PLO5_ERR_RANKS;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t i = (uint32_t)(pct / 100.0 * (double)g_bt2_n);
    if (i >= g_bt2_n) i = g_bt2_n - 1;
    for (int k = 0; k < 5; k++) out[k] = g_bt2_cards[i][k];
    return PLO5_OK;
}

int plo5_board_ranks_state(int board_out[5], int *nboard_out)
{
    if (!g_bt_nboard) return 0;
    if (board_out) memcpy(board_out, g_bt_board, (size_t)g_bt_nboard * sizeof(int));
    if (nboard_out) *nboard_out = g_bt_nboard;
    return 1;
}

double plo5_hand_percentile_on(const int cards[5], const int *board, int nboard)
{
    if (nboard == 0) return plo5_hand_percentile(cards);
    if (!board_match(board, nboard)) return -1.0;
    int c[5];
    memcpy(c, cards, sizeof c);
    for (int i = 0; i < 5; i++) if (c[i] < 0 || c[i] > 51) return -1.0;
    sort5(c);
    uint32_t r = g_bt_of_colex[colex5(c)];
    if (r == 0xFFFFFFFFu) return -1.0;      /* holding uses a board card */
    return (r + 0.5) * 100.0 / (double)g_bt_n;
}

int plo5_percentile_hand_on(double pct, const int *board, int nboard, int out[5])
{
    if (nboard == 0) return plo5_percentile_hand(pct, out);
    if (!board_match(board, nboard)) return PLO5_ERR_RANKS;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t i = (uint32_t)(pct / 100.0 * (double)g_bt_n);
    if (i >= g_bt_n) i = g_bt_n - 1;
    for (int k = 0; k < 5; k++) out[k] = g_bt_cards[i][k];
    return PLO5_OK;
}

/* ------------------------------------------------------------------ */
/* Simulation                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int      nplayers;
    int      nboard;                 /* known board cards */
    uint32_t bkey[5];                /* keys of known board cards */
    int      db;                     /* double-board split pot */
    int      nboard2;
    uint32_t bkey2[5];
    combo_t  ppairs[PLO5_MAX_PLAYERS][10];
    int      ptype[PLO5_MAX_PLAYERS];
    double   plo_[PLO5_MAX_PLAYERS], phi_[PLO5_MAX_PLAYERS];
    uint32_t rlo[PLO5_MAX_PLAYERS], rhi[PLO5_MAX_PLAYERS]; /* range slice */
    int      rtable;                 /* 0 preflop, 1 board, 2 two-board */
    int      nrand, nrange;
    int      avail[52];
    int      navail;
    uint64_t base_used;
} setup_t;

typedef struct {
    const setup_t *st;
    uint64_t trials;
    uint64_t seed;
    double   sum[PLO5_MAX_PLAYERS], sum2[PLO5_MAX_PLAYERS];
    uint64_t nwin[PLO5_MAX_PLAYERS], ntie[PLO5_MAX_PLAYERS];
    int      failed;                 /* range sampling gave up */
    int      deck[52];
} worker_t;

static void score_board(const setup_t *st, const uint32_t bkey[5],
                        combo_t rp[][10],
                        double *sum, double *sum2,
                        uint64_t *nwin, uint64_t *ntie)
{
    combo_t bt[10];
    make_trips(bkey, bt);

    uint32_t bv[PLO5_MAX_PLAYERS], best = 0;
    int cnt = 0, np = st->nplayers;
    for (int p = 0; p < np; p++) {
        const combo_t *hp = st->ptype[p] == PLO5_P_FIXED ? st->ppairs[p] : rp[p];
        uint32_t v = best_val(hp, bt);
        bv[p] = v;
        if (v > best) { best = v; cnt = 1; }
        else if (v == best) cnt++;
    }
    double share = 1.0 / (double)cnt;
    for (int p = 0; p < np; p++)
        if (bv[p] == best) {
            sum[p]  += share;
            sum2[p] += share * share;
            if (cnt == 1) nwin[p]++; else ntie[p]++;
        }
}

/* double-board scoring: each board pays half the pot to its best hand(s) */
static void score_board_db(const setup_t *st, const uint32_t bkA[5],
                           const uint32_t bkB[5], combo_t rp[][10],
                           double *sum, double *sum2,
                           uint64_t *nwin, uint64_t *ntie)
{
    combo_t btA[10], btB[10];
    make_trips(bkA, btA);
    make_trips(bkB, btB);

    uint32_t vA[PLO5_MAX_PLAYERS], vB[PLO5_MAX_PLAYERS];
    uint32_t bestA = 0, bestB = 0;
    int cA = 0, cB = 0, np = st->nplayers;
    for (int p = 0; p < np; p++) {
        const combo_t *hp = st->ptype[p] == PLO5_P_FIXED ? st->ppairs[p] : rp[p];
        vA[p] = best_val(hp, btA);
        vB[p] = best_val(hp, btB);
        if (vA[p] > bestA) { bestA = vA[p]; cA = 1; }
        else if (vA[p] == bestA) cA++;
        if (vB[p] > bestB) { bestB = vB[p]; cB = 1; }
        else if (vB[p] == bestB) cB++;
    }
    for (int p = 0; p < np; p++) {
        int inA = vA[p] == bestA, inB = vB[p] == bestB;
        if (!inA && !inB) continue;
        double share = (inA ? 0.5 / cA : 0.0) + (inB ? 0.5 / cB : 0.0);
        sum[p]  += share;
        sum2[p] += share * share;
        if (inA && inB && cA == 1 && cB == 1) nwin[p]++;  /* clean scoop */
        else ntie[p]++;
    }
}

static void run_mc(worker_t *w)
{
    const setup_t *st = w->st;
    rng_t rng;
    rng_seed(&rng, w->seed);

    memcpy(w->deck, st->avail, (size_t)st->navail * sizeof(int));
    int n = st->navail;
    int need = 5 - st->nboard;
    int need2 = st->db ? 5 - st->nboard2 : 0;
    int ndraw = need + need2 + 5 * st->nrand;

    uint32_t bkey[5], bkey2[5];
    memcpy(bkey, st->bkey, sizeof bkey);
    memcpy(bkey2, st->bkey2, sizeof bkey2);
    combo_t rpairs[PLO5_MAX_PLAYERS][10];

    if (st->nrange == 0) {
        /* no range players: partial Fisher-Yates draw */
        for (uint64_t t = 0; t < w->trials; t++) {
            for (int i = 0; i < ndraw; i++) {
                int j = i + (int)rng_below(&rng, (uint32_t)(n - i));
                int tmp = w->deck[i]; w->deck[i] = w->deck[j]; w->deck[j] = tmp;
            }
            for (int i = 0; i < need; i++)
                bkey[st->nboard + i] = card_key[w->deck[i]];
            for (int i = 0; i < need2; i++)
                bkey2[st->nboard2 + i] = card_key[w->deck[need + i]];
            int di = need + need2;
            for (int p = 0; p < st->nplayers; p++)
                if (st->ptype[p] == PLO5_P_RANDOM) {
                    uint32_t hk[5];
                    for (int i = 0; i < 5; i++) hk[i] = card_key[w->deck[di++]];
                    make_pairs(hk, rpairs[p]);
                }
            if (st->db)
                score_board_db(st, bkey, bkey2, rpairs,
                               w->sum, w->sum2, w->nwin, w->ntie);
            else
                score_board(st, bkey, rpairs, w->sum, w->sum2, w->nwin, w->ntie);
        }
        return;
    }

    /* range players: sample their combo from the rank-table slice by
     * rejection against already-used cards, then fill boards and random
     * hands from the remaining deck (also by rejection) */
    for (uint64_t t = 0; t < w->trials; t++) {
        uint64_t used = st->base_used;

        for (int p = 0; p < st->nplayers; p++) {
            if (st->ptype[p] != PLO5_P_RANGE) continue;
            uint32_t span = st->rhi[p] - st->rlo[p];
            int tries = 0;
            for (;;) {
                uint32_t ix = st->rlo[p] + rng_below(&rng, span);
                const uint8_t *cd = st->rtable == 2 ? g_bt2_cards[ix] :
                                    st->rtable == 1 ? g_bt_cards[ix] :
                                                      g_rk_cards[ix];
                uint64_t m = (1ull << cd[0]) | (1ull << cd[1]) | (1ull << cd[2]) |
                             (1ull << cd[3]) | (1ull << cd[4]);
                if (!(m & used)) {
                    used |= m;
                    uint32_t hk[5];
                    for (int i = 0; i < 5; i++) hk[i] = card_key[cd[i]];
                    make_pairs(hk, rpairs[p]);
                    break;
                }
                if (++tries >= 100000) { w->failed = 1; return; }
            }
        }
        for (int i = 0; i < need; i++) {
            for (;;) {
                int cx = w->deck[rng_below(&rng, (uint32_t)n)];
                uint64_t m = 1ull << cx;
                if (!(m & used)) {
                    used |= m;
                    bkey[st->nboard + i] = card_key[cx];
                    break;
                }
            }
        }
        for (int i = 0; i < need2; i++) {
            for (;;) {
                int cx = w->deck[rng_below(&rng, (uint32_t)n)];
                uint64_t m = 1ull << cx;
                if (!(m & used)) {
                    used |= m;
                    bkey2[st->nboard2 + i] = card_key[cx];
                    break;
                }
            }
        }
        for (int p = 0; p < st->nplayers; p++) {
            if (st->ptype[p] != PLO5_P_RANDOM) continue;
            uint32_t hk[5];
            for (int i = 0; i < 5; i++) {
                for (;;) {
                    int cx = w->deck[rng_below(&rng, (uint32_t)n)];
                    uint64_t m = 1ull << cx;
                    if (!(m & used)) { used |= m; hk[i] = card_key[cx]; break; }
                }
            }
            make_pairs(hk, rpairs[p]);
        }
        if (st->db)
            score_board_db(st, bkey, bkey2, rpairs,
                           w->sum, w->sum2, w->nwin, w->ntie);
        else
            score_board(st, bkey, rpairs, w->sum, w->sum2, w->nwin, w->ntie);
    }
}

static void run_exact(const setup_t *st, worker_t *w)
{
    int k = 5 - st->nboard, n = st->navail;
    uint32_t bkey[5];
    memcpy(bkey, st->bkey, sizeof bkey);
    combo_t dummy[PLO5_MAX_PLAYERS][10];

    if (k == 0) {
        score_board(st, bkey, dummy, w->sum, w->sum2, w->nwin, w->ntie);
        w->trials = 1;
        return;
    }

    int idx[5];
    for (int i = 0; i < k; i++) idx[i] = i;
    uint64_t count = 0;
    for (;;) {
        for (int i = 0; i < k; i++)
            bkey[st->nboard + i] = card_key[st->avail[idx[i]]];
        score_board(st, bkey, dummy, w->sum, w->sum2, w->nwin, w->ntie);
        count++;

        int i = k - 1;
        while (i >= 0 && idx[i] == n - k + i) i--;
        if (i < 0) break;
        idx[i]++;
        for (int j = i + 1; j < k; j++) idx[j] = idx[j - 1] + 1;
    }
    w->trials = count;
}

/* advance a k-combination over [0,n); returns 0 when exhausted */
static int comb_next(int *idx, int k, int n)
{
    int i = k - 1;
    while (i >= 0 && idx[i] == n - k + i) i--;
    if (i < 0) return 0;
    idx[i]++;
    for (int j = i + 1; j < k; j++) idx[j] = idx[j - 1] + 1;
    return 1;
}

static void run_exact_db(const setup_t *st, worker_t *w)
{
    int k1 = 5 - st->nboard, k2 = 5 - st->nboard2, n = st->navail;
    uint32_t bkA[5], bkB[5];
    memcpy(bkA, st->bkey, sizeof bkA);
    memcpy(bkB, st->bkey2, sizeof bkB);
    combo_t dummy[PLO5_MAX_PLAYERS][10];
    uint64_t count = 0;

    int i1[5];
    for (int i = 0; i < k1; i++) i1[i] = i;
    do {
        for (int i = 0; i < k1; i++)
            bkA[st->nboard + i] = card_key[st->avail[i1[i]]];
        /* remaining deck for board B */
        int rem[52], nr = 0, s = 0;
        for (int i = 0; i < n; i++) {
            if (s < k1 && i1[s] == i) { s++; continue; }
            rem[nr++] = st->avail[i];
        }
        int i2[5];
        for (int i = 0; i < k2; i++) i2[i] = i;
        do {
            for (int i = 0; i < k2; i++)
                bkB[st->nboard2 + i] = card_key[rem[i2[i]]];
            score_board_db(st, bkA, bkB, dummy,
                           w->sum, w->sum2, w->nwin, w->ntie);
            count++;
        } while (k2 > 0 && comb_next(i2, k2, nr));
    } while (k1 > 0 && comb_next(i1, k1, n));
    w->trials = count;
}

/* ------------------------------------------------------------------ */
/* Threads (Win32 on Windows, pthreads elsewhere)                      */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) { run_mc((worker_t *)p); return 0; }
#else
static void *thread_main(void *p) { run_mc((worker_t *)p); return NULL; }
#endif

static void run_mc_threads(worker_t *ws, int nt)
{
    if (nt <= 1) { run_mc(&ws[0]); return; }
#ifdef _WIN32
    HANDLE th[64];
    for (int i = 0; i < nt; i++) {
        th[i] = CreateThread(NULL, 0, thread_main, &ws[i], 0, NULL);
        if (!th[i]) { run_mc(&ws[i]); }        /* degrade gracefully */
    }
    for (int i = 0; i < nt; i++)
        if (th[i]) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
#else
    pthread_t th[64];
    int ok[64];
    for (int i = 0; i < nt; i++) {
        ok[i] = pthread_create(&th[i], NULL, thread_main, &ws[i]) == 0;
        if (!ok[i]) run_mc(&ws[i]);
    }
    for (int i = 0; i < nt; i++)
        if (ok[i]) pthread_join(th[i], NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static uint64_t choose64(int n, int k)
{
    if (k < 0 || k > n) return 0;
    uint64_t r = 1;
    for (int i = 1; i <= k; i++) r = r * (uint64_t)(n - k + i) / (uint64_t)i;
    return r;
}

static int take_card(int c, uint64_t *used)
{
    if (c < 0 || c > 51) return PLO5_ERR_CARD;
    if (*used >> c & 1)  return PLO5_ERR_DUP;
    *used |= 1ull << c;
    return PLO5_OK;
}

static int equity_core(const plo5_player *players, int nplayers,
                       const int *board, int nboard,
                       const int *board2, int nboard2, int db,
                       const int *dead, int ndead,
                       uint64_t trials, uint64_t max_enum,
                       uint64_t seed, int nthreads,
                       plo5_result *out)
{
    if (!initialized) plo5_init();
    if (!players || !out) return PLO5_ERR_ARG;
    if (nplayers < 2 || nplayers > PLO5_MAX_PLAYERS) return PLO5_ERR_PLAYERS;
    if (nboard != 0 && nboard != 3 && nboard != 4 && nboard != 5)
        return PLO5_ERR_BOARD;
    if (db && nboard2 != 0 && nboard2 != 3 && nboard2 != 4 && nboard2 != 5)
        return PLO5_ERR_BOARD;
    if ((nboard > 0 && !board) || (ndead > 0 && !dead) ||
        (db && nboard2 > 0 && !board2)) return PLO5_ERR_ARG;

    memset(out, 0, sizeof *out);

    setup_t st;
    memset(&st, 0, sizeof st);
    st.nplayers = nplayers;
    st.nboard = nboard;
    st.db = db;
    st.nboard2 = db ? nboard2 : 0;

    uint64_t used = 0;
    int rc;

    for (int p = 0; p < nplayers; p++) {
        const plo5_player *pl = &players[p];
        st.ptype[p] = pl->type;
        switch (pl->type) {
        case PLO5_P_FIXED: {
            uint32_t hk[5];
            for (int i = 0; i < 5; i++) {
                if ((rc = take_card(pl->cards[i], &used)) != PLO5_OK) return rc;
                hk[i] = card_key[pl->cards[i]];
            }
            make_pairs(hk, st.ppairs[p]);
            break;
        }
        case PLO5_P_RANDOM:
            st.nrand++;
            break;
        case PLO5_P_RANGE: {
            double lo = pl->lo, hi = pl->hi;
            if (lo < 0) lo = 0;
            if (hi > 100) hi = 100;
            if (hi <= lo) return PLO5_ERR_ARG;
            st.plo_[p] = lo;
            st.phi_[p] = hi;
            st.nrange++;
            break;
        }
        default:
            return PLO5_ERR_ARG;
        }
    }

    for (int i = 0; i < nboard; i++) {
        if ((rc = take_card(board[i], &used)) != PLO5_OK) return rc;
        st.bkey[i] = card_key[board[i]];
    }
    for (int i = 0; i < st.nboard2; i++) {
        if ((rc = take_card(board2[i], &used)) != PLO5_OK) return rc;
        st.bkey2[i] = card_key[board2[i]];
    }
    for (int i = 0; i < ndead; i++)
        if ((rc = take_card(dead[i], &used)) != PLO5_OK) return rc;

    st.base_used = used;
    for (int c = 0; c < 52; c++)
        if (!(used >> c & 1)) st.avail[st.navail++] = c;

    /* range players: pick the strength distribution — the static preflop
     * table when there is no board, else the board-conditional table
     * (built on demand and cached per board); double board needs both
     * boards ranked (or neither) */
    if (st.nrange > 0) {
        uint32_t table_n;
        if (db) {
            if (nboard == 0 && st.nboard2 == 0) {
                if (!g_rk_cards) return PLO5_ERR_RANKS;
                st.rtable = 0;
                table_n = NCOMBO;
            } else if (nboard >= 3 && st.nboard2 >= 3) {
                int rc2 = plo5_board_ranks_build2(board, nboard,
                                                  board2, st.nboard2,
                                                  0, nthreads, NULL, NULL);
                if (rc2 != PLO5_OK) return rc2;
                st.rtable = 2;
                table_n = g_bt2_n;
            } else {
                return PLO5_ERR_RANKS;
            }
        } else if (nboard > 0) {
            int rc2 = plo5_board_ranks_build(board, nboard, 0, nthreads,
                                             NULL, NULL);
            if (rc2 != PLO5_OK) return rc2;
            st.rtable = 1;
            table_n = g_bt_n;
        } else {
            if (!g_rk_cards) return PLO5_ERR_RANKS;
            st.rtable = 0;
            table_n = NCOMBO;
        }
        for (int p = 0; p < nplayers; p++) {
            if (st.ptype[p] != PLO5_P_RANGE) continue;
            st.rlo[p] = (uint32_t)(st.plo_[p] / 100.0 * (double)table_n);
            st.rhi[p] = (uint32_t)(st.phi_[p] / 100.0 * (double)table_n);
            if (st.rhi[p] > table_n) st.rhi[p] = table_n;
            if (st.rhi[p] <= st.rlo[p]) st.rhi[p] = st.rlo[p] + 1;
        }
    }

    int need = 5 - nboard;
    int need2 = db ? 5 - st.nboard2 : 0;
    int ndraw = need + need2 + 5 * (st.nrand + st.nrange);
    if (st.navail < ndraw) return PLO5_ERR_DECK;

    uint64_t combos = choose64(st.navail, need) *
                      choose64(st.navail - need, need2);
    int exact = (st.nrand == 0) && (st.nrange == 0) &&
                combos > 0 && combos <= max_enum;

    if (exact) {
        worker_t *w = calloc(1, sizeof *w);
        if (!w) return PLO5_ERR_ARG;
        w->st = &st;
        if (db) run_exact_db(&st, w);
        else    run_exact(&st, w);
        out->exact = 1;
        out->samples = w->trials;
        for (int p = 0; p < nplayers; p++) {
            double n = (double)w->trials;
            out->equity[p] = w->sum[p] / n;
            out->win[p]    = (double)w->nwin[p] / n;
            out->tie[p]    = (double)w->ntie[p] / n;
            out->ci95[p]   = 0.0;
        }
        free(w);
        return PLO5_OK;
    }

    if (trials == 0) return PLO5_ERR_TRIALS;

    int nt = nthreads < 1 ? 1 : nthreads;
    if (nt > 64) nt = 64;
    if ((uint64_t)nt > trials) nt = (int)trials;

    worker_t *ws = calloc((size_t)nt, sizeof *ws);
    if (!ws) return PLO5_ERR_ARG;

    uint64_t per = trials / (uint64_t)nt, rem = trials % (uint64_t)nt;
    for (int i = 0; i < nt; i++) {
        ws[i].st = &st;
        ws[i].trials = per + (i == 0 ? rem : 0);
        uint64_t s = seed + (uint64_t)i;
        ws[i].seed = splitmix64(&s);
    }
    run_mc_threads(ws, nt);

    for (int i = 0; i < nt; i++)
        if (ws[i].failed) { free(ws); return PLO5_ERR_RANGE; }

    double sum[PLO5_MAX_PLAYERS] = {0}, sum2[PLO5_MAX_PLAYERS] = {0};
    uint64_t nwin[PLO5_MAX_PLAYERS] = {0}, ntie[PLO5_MAX_PLAYERS] = {0};
    uint64_t total = 0;
    for (int i = 0; i < nt; i++) {
        total += ws[i].trials;
        for (int p = 0; p < nplayers; p++) {
            sum[p]  += ws[i].sum[p];
            sum2[p] += ws[i].sum2[p];
            nwin[p] += ws[i].nwin[p];
            ntie[p] += ws[i].ntie[p];
        }
    }
    free(ws);

    out->exact = 0;
    out->samples = total;
    double n = (double)total;
    for (int p = 0; p < nplayers; p++) {
        double eq  = sum[p] / n;
        double var = sum2[p] / n - eq * eq;
        if (var < 0) var = 0;
        out->equity[p] = eq;
        out->win[p]    = (double)nwin[p] / n;
        out->tie[p]    = (double)ntie[p] / n;
        out->ci95[p]   = 1.96 * sqrt(var / n);
    }
    return PLO5_OK;
}

int plo5_equity2(const plo5_player *players, int nplayers,
                 const int *board, int nboard,
                 const int *dead, int ndead,
                 uint64_t trials, uint64_t max_enum,
                 uint64_t seed, int nthreads,
                 plo5_result *out)
{
    return equity_core(players, nplayers, board, nboard, NULL, 0, 0,
                       dead, ndead, trials, max_enum, seed, nthreads, out);
}

int plo5_equity_2b(const plo5_player *players, int nplayers,
                   const int *board1, int nboard1,
                   const int *board2, int nboard2,
                   const int *dead, int ndead,
                   uint64_t trials, uint64_t max_enum,
                   uint64_t seed, int nthreads,
                   plo5_result *out)
{
    return equity_core(players, nplayers, board1, nboard1, board2, nboard2, 1,
                       dead, ndead, trials, max_enum, seed, nthreads, out);
}

int plo5_equity(const int *hands, int nplayers,
                const int *board, int nboard,
                const int *dead, int ndead,
                uint64_t trials, uint64_t max_enum,
                uint64_t seed, int nthreads,
                plo5_result *out)
{
    if (!hands) return PLO5_ERR_ARG;
    if (nplayers < 2 || nplayers > PLO5_MAX_PLAYERS) return PLO5_ERR_PLAYERS;

    plo5_player pl[PLO5_MAX_PLAYERS];
    memset(pl, 0, sizeof pl);
    for (int p = 0; p < nplayers; p++) {
        const int *h = hands + p * 5;
        int nrandc = 0;
        for (int i = 0; i < 5; i++) if (h[i] == PLO5_RANDOM) nrandc++;
        if (nrandc == 5) {
            pl[p].type = PLO5_P_RANDOM;
        } else if (nrandc != 0) {
            return PLO5_ERR_CARD;   /* partially random not supported */
        } else {
            pl[p].type = PLO5_P_FIXED;
            for (int i = 0; i < 5; i++) pl[p].cards[i] = h[i];
        }
    }
    return plo5_equity2(pl, nplayers, board, nboard, dead, ndead,
                        trials, max_enum, seed, nthreads, out);
}

/* ------------------------------------------------------------------ */
/* Card text helpers                                                   */
/* ------------------------------------------------------------------ */

static const char RANK_CH[] = "23456789TJQKA";
static const char SUIT_CH[] = "cdhs";

int plo5_parse_card(const char *s)
{
    if (!s || !s[0] || !s[1]) return -1;
    char rc = s[0], sc = s[1];
    if (rc >= 'a' && rc <= 'z') rc = (char)(rc - 'a' + 'A');
    if (sc >= 'A' && sc <= 'Z') sc = (char)(sc - 'A' + 'a');
    int r = -1, u = -1;
    for (int i = 0; i < 13; i++) if (RANK_CH[i] == rc) { r = i; break; }
    for (int i = 0; i < 4; i++)  if (SUIT_CH[i] == sc) { u = i; break; }
    if (r < 0 || u < 0) return -1;
    return r * 4 + u;
}

void plo5_card_str(int id, char out[3])
{
    if (id < 0 || id > 51) { out[0] = '?'; out[1] = '?'; out[2] = 0; return; }
    out[0] = RANK_CH[id >> 2];
    out[1] = SUIT_CH[id & 3];
    out[2] = 0;
}
