/* api.c — transport-independent JSON API over the PLO5 engine. See api.h.
 *
 * Endpoints (all take a URL-encoded query string):
 *   /api/equity   players=SPEC,SPEC[,..]&board=..&board2=..&double=0/1
 *                 &dead=..&trials=N&maxenum=N&seed=N&buildranks=0/1
 *   /api/lookup   pct=30&board=..&board2=..        -> hand at that pct
 *   /api/rankof   hand=..&board=..&board2=..       -> pct of a hand
 *   /api/findeq   target=55&board=..&board2=..&double=0/1
 *   /api/random   street=3..5&count=1/2&used=..    -> random board(s)
 *   /api/sprquiz  board=..&spr=..&opp=N&offset=PP&pot=..&trials=..
 *   /api/status
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "api.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plo5.h"
#include "spr.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static int g_ncpu = 1;
/* Fast/Balanced/Precise trial presets served to the UI; plo5setup
 * calibrates these to the machine via plo5config.ini */
static uint64_t g_presets[3] = { 200000, 1000000, 10000000 };

/* ------------------------------------------------------------------ */
/* Platform helpers                                                    */
/* ------------------------------------------------------------------ */

double plo5_api_now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

uint64_t plo5_api_seed(void)
{
    uint64_t s;
#ifdef _WIN32
    LARGE_INTEGER qc;
    QueryPerformanceCounter(&qc);
    s = (uint64_t)qc.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    s = (uint64_t)ts.tv_sec * 1000000007ull ^ (uint64_t)ts.tv_nsec
        ^ ((uint64_t)getpid() << 32);
#endif
    s ^= s >> 33; s *= 0xff51afd7ed558ccdull; s ^= s >> 33;
    return s ? s : 0x9E3779B97F4A7C15ull;
}

int plo5_api_ncpu(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n < 1 ? 1 : (int)n;
}

void plo5_api_init(int nthreads, const uint64_t presets[3])
{
    g_ncpu = nthreads > 0 ? nthreads : plo5_api_ncpu();
    if (presets)
        for (int i = 0; i < 3; i++)
            if (presets[i] > 0) g_presets[i] = presets[i];
}

/* ------------------------------------------------------------------ */
/* String buffer                                                       */
/* ------------------------------------------------------------------ */

void plo5_sb_init(plo5_sb *s) { s->buf = NULL; s->len = s->cap = 0; }
void plo5_sb_free(plo5_sb *s) { free(s->buf); plo5_sb_init(s); }

void plo5_sb_put(plo5_sb *s, const char *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        while (nc < s->len + n + 1) nc *= 2;
        char *nb = realloc(s->buf, nc);
        if (!nb) return;
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = 0;
}

void plo5_sb_printf(plo5_sb *s, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) plo5_sb_put(s, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

#define sb_lit(s, lit) plo5_sb_put(s, lit, strlen(lit))

/* ------------------------------------------------------------------ */
/* Query parsing                                                       */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int plo5_api_qget(const char *query, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vn = seg - klen - 1, o = 0;
            for (size_t i = 0; i < vn && o + 1 < cap; i++) {
                char c = v[i];
                if (c == '+') c = ' ';
                else if (c == '%' && i + 2 < vn) {
                    int h1 = hexval(v[i + 1]), h2 = hexval(v[i + 2]);
                    if (h1 >= 0 && h2 >= 0) { c = (char)(h1 * 16 + h2); i += 2; }
                }
                out[o++] = c;
            }
            out[o] = 0;
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (cap) out[0] = 0;
    return 0;
}

static double qnum(const char *query, const char *key, double dflt)
{
    char v[64];
    if (!plo5_api_qget(query, key, v, sizeof v) || !v[0]) return dflt;
    return atof(v);
}

/* ------------------------------------------------------------------ */
/* Hand spec parsing (same syntax as the CLI)                          */
/* ------------------------------------------------------------------ */

static int parse_cards(const char *s, int *out, int maxn)
{
    int n = 0, bi = 0;
    char buf[3] = { 0, 0, 0 };
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
        char x = (char)tolower((unsigned char)*a), y = (char)tolower((unsigned char)*b);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

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
        const char *at = s;
        for (int i = 0; i < nseg; i++) {
            if (parse_band(at, seg[i], &lo[i], &hi[i]) != 0) return -1;
            at = seg[i] + 1;
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

const char *plo5_api_err_str(int rc)
{
    switch (rc) {
    case PLO5_ERR_PLAYERS: return "need 2..6 players";
    case PLO5_ERR_BOARD:   return "board must have 0, 3, 4 or 5 cards";
    case PLO5_ERR_CARD:    return "invalid card";
    case PLO5_ERR_DUP:     return "duplicate card";
    case PLO5_ERR_DECK:    return "not enough cards left in deck";
    case PLO5_ERR_TRIALS:  return "no trials and enumeration not possible";
    case PLO5_ERR_RANKS:   return "preflop rank table missing - build it once (plo5setup, or plo5calc --gen-ranks)";
    case PLO5_ERR_IO:      return "cannot read/write rank file";
    case PLO5_ERR_RANGE:   return "range cannot be dealt with these dead cards";
    default:               return "invalid arguments";
    }
}

static int json_error(plo5_sb *sb, const char *msg)
{
    plo5_sb_printf(sb, "{\"error\":\"%s\"}", msg);
    return 400;
}

static void put_dbl_array(plo5_sb *sb, const double *v, int n, double scale)
{
    sb_lit(sb, "[");
    for (int i = 0; i < n; i++)
        plo5_sb_printf(sb, "%s%.6f", i ? "," : "", v[i] * scale);
    sb_lit(sb, "]");
}

/* cards -> "AhKd.." sorted strongest first, for display */
static void hand_str_sorted(const int *h, int n, char *out)
{
    int t[5];
    memcpy(t, h, sizeof(int) * (size_t)n);
    for (int i = 1; i < n; i++) {
        int v = t[i], j = i - 1;
        while (j >= 0 && t[j] < v) { t[j + 1] = t[j]; j--; }
        t[j + 1] = v;
    }
    char cs[3];
    out[0] = 0;
    for (int i = 0; i < n; i++) { plo5_card_str(t[i], cs); strcat(out, cs); }
}

static void cards_str(const int *c, int n, char *out)
{
    char cs[3];
    out[0] = 0;
    for (int i = 0; i < n; i++) { plo5_card_str(c[i], cs); strcat(out, cs); }
}

/* ------------------------------------------------------------------ */
/* Progress plumbing: engine callbacks -> caller's labelled callback   */
/* ------------------------------------------------------------------ */

static plo5_api_progress_fn g_prog;
static void *g_prog_ud;
static const char *g_prog_what = "";

static void prog_tramp(int done, int total, void *ud)
{
    (void)ud;
    if (g_prog) g_prog(g_prog_what, done, total, g_prog_ud);
}

typedef void (*eng_prog_fn)(int, int, void *);

static eng_prog_fn prog_for(const char *what)
{
    g_prog_what = what;
    return g_prog ? prog_tramp : NULL;
}

static const char *street_label(int nb)
{
    return nb == 3 ? "flop ranking" : nb == 4 ? "turn ranking" : "river ranking";
}

static int build_single(const int *board, int nb)
{
    return plo5_board_ranks_build(board, nb, 100, g_ncpu,
                                  prog_for(street_label(nb)), NULL);
}

static int build_double(const int *b1, int nb1, const int *b2, int nb2)
{
    return plo5_board_ranks_build2(b1, nb1, b2, nb2, 100, g_ncpu,
                                   prog_for("two-board ranking"), NULL);
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static int api_equity(const char *q, plo5_sb *sb)
{
    char pspec[512], bstr[64], b2str[64], dstr[128];
    if (!plo5_api_qget(q, "players", pspec, sizeof pspec) || !pspec[0])
        return json_error(sb, "players parameter required");
    plo5_api_qget(q, "board", bstr, sizeof bstr);
    plo5_api_qget(q, "board2", b2str, sizeof b2str);
    plo5_api_qget(q, "dead", dstr, sizeof dstr);

    plo5_player pl[PLO5_MAX_PLAYERS];
    int np = 0;
    for (char *tok = pspec; tok && *tok; ) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = 0;
        if (np >= PLO5_MAX_PLAYERS) return json_error(sb, "too many players");
        if (parse_player(tok, &pl[np]) != 0)
            return json_error(sb, "cannot parse a player spec");
        np++;
        tok = comma ? comma + 1 : NULL;
    }

    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    int dead[40];
    int nd = parse_cards(dstr, dead, 40);
    if (nb < 0 || nb2 < 0 || nd < 0) return json_error(sb, "bad card list");

    int dbl = (int)qnum(q, "double", 0) || nb2 > 0;
    uint64_t trials = (uint64_t)qnum(q, "trials", 1000000);
    uint64_t maxen = (uint64_t)qnum(q, "maxenum", 1000000);
    uint64_t seed = (uint64_t)qnum(q, "seed", 0);
    if (!seed) seed = plo5_api_seed();

    /* optionally (re)build board rankings so percentiles come back filled */
    if ((int)qnum(q, "buildranks", 0)) {
        if (dbl && nb >= 3 && nb2 >= 3) build_double(board, nb, board2, nb2);
        else if (!dbl && nb >= 3)       build_single(board, nb);
    }

    plo5_result r;
    double t0 = plo5_api_now_ms();
    int rc = dbl
        ? plo5_equity_2b(pl, np, board, nb, board2, nb2, dead, nd,
                         trials, maxen, seed, g_ncpu, &r)
        : plo5_equity2(pl, np, board, nb, dead, nd,
                       trials, maxen, seed, g_ncpu, &r);
    double ms = plo5_api_now_ms() - t0;
    if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));

    plo5_sb_printf(sb, "{\"samples\":%llu,\"exact\":%s,\"ms\":%.1f,\"double\":%s,",
                   (unsigned long long)r.samples, r.exact ? "true" : "false",
                   ms, dbl ? "true" : "false");
    sb_lit(sb, "\"equity\":"); put_dbl_array(sb, r.equity, np, 100);
    sb_lit(sb, ",\"win\":");   put_dbl_array(sb, r.win, np, 100);
    sb_lit(sb, ",\"tie\":");   put_dbl_array(sb, r.tie, np, 100);
    sb_lit(sb, ",\"ci95\":");  put_dbl_array(sb, r.ci95, np, 100);

    sb_lit(sb, ",\"handType\":[");
    for (int p = 0; p < np; p++) {
        if (p) sb_lit(sb, ",");
        put_dbl_array(sb, r.hand_type[p], PLO5_NCAT, 100);
    }
    sb_lit(sb, "]");
    if (dbl) {
        sb_lit(sb, ",\"handTypeB\":[");
        for (int p = 0; p < np; p++) {
            if (p) sb_lit(sb, ",");
            put_dbl_array(sb, r.hand_type_b[p], PLO5_NCAT, 100);
        }
        sb_lit(sb, "]");
    }

    /* percentile of each fixed hand, when the matching table is available */
    sb_lit(sb, ",\"pct\":[");
    for (int p = 0; p < np; p++) {
        double pc = -1;
        if (pl[p].type == PLO5_P_FIXED) {
            if (dbl) {
                if (nb >= 3 && nb2 >= 3)
                    pc = plo5_hand_percentile_2b(pl[p].cards, board, nb, board2, nb2);
                else if (nb == 0 && nb2 == 0)
                    pc = plo5_hand_percentile(pl[p].cards);
            } else {
                pc = plo5_hand_percentile_on(pl[p].cards, board, nb);
            }
        }
        if (pc >= 0) plo5_sb_printf(sb, "%s%.1f", p ? "," : "", pc);
        else plo5_sb_printf(sb, "%snull", p ? "," : "");
    }
    plo5_sb_put(sb, "]}", 2);
    return 200;
}

static int api_lookup(const char *q, plo5_sb *sb)
{
    char bstr[64], b2str[64];
    plo5_api_qget(q, "board", bstr, sizeof bstr);
    plo5_api_qget(q, "board2", b2str, sizeof b2str);
    double pct = qnum(q, "pct", 50);
    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) return json_error(sb, "bad card list");

    int h[5], rc;
    const char *street;
    if (nb2 >= 3 && nb >= 3) {
        rc = build_double(board, nb, board2, nb2);
        if (rc == PLO5_OK)
            rc = plo5_percentile_hand_2b(pct, board, nb, board2, nb2, h);
        street = "both boards";
    } else if (nb >= 3) {
        rc = build_single(board, nb);
        if (rc == PLO5_OK)
            rc = plo5_percentile_hand_on(pct, board, nb, h);
        street = nb == 3 ? "flop" : nb == 4 ? "turn" : "river";
    } else {
        rc = plo5_percentile_hand(pct, h);
        street = "preflop";
    }
    if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));

    char hs[16];
    hand_str_sorted(h, 5, hs);
    plo5_sb_printf(sb, "{\"hand\":\"%s\",\"street\":\"%s\"}", hs, street);
    return 200;
}

static int api_rankof(const char *q, plo5_sb *sb)
{
    char hstr[32], bstr[64], b2str[64];
    if (!plo5_api_qget(q, "hand", hstr, sizeof hstr)) return json_error(sb, "hand required");
    plo5_api_qget(q, "board", bstr, sizeof bstr);
    plo5_api_qget(q, "board2", b2str, sizeof b2str);
    int h[5], board[5], board2[5];
    if (parse_cards(hstr, h, 5) != 5) return json_error(sb, "bad hand");
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) return json_error(sb, "bad card list");

    double pc;
    if (nb >= 3 && nb2 >= 3) {
        build_double(board, nb, board2, nb2);
        pc = plo5_hand_percentile_2b(h, board, nb, board2, nb2);
    } else if (nb >= 3) {
        build_single(board, nb);
        pc = plo5_hand_percentile_on(h, board, nb);
    } else {
        pc = plo5_hand_percentile(h);
    }
    if (pc < 0) return json_error(sb, "percentile unavailable (table missing or card conflict)");
    plo5_sb_printf(sb, "{\"pct\":%.2f}", pc);
    return 200;
}

/* binary-search the strength distribution for a hand with the target
 * equity vs one random opponent (same algorithm as the desktop GUI) */
static int api_findeq(const char *q, plo5_sb *sb)
{
    char bstr[64], b2str[64];
    plo5_api_qget(q, "board", bstr, sizeof bstr);
    plo5_api_qget(q, "board2", b2str, sizeof b2str);
    double target = qnum(q, "target", 55) / 100.0;
    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) return json_error(sb, "bad card list");
    int dbl = (int)qnum(q, "double", 0) || nb2 > 0;

    int rc = PLO5_OK;
    if (dbl && nb >= 3 && nb2 >= 3) rc = build_double(board, nb, board2, nb2);
    else if (!dbl && nb >= 3)       rc = build_single(board, nb);
    else if (!plo5_ranks_loaded())  rc = PLO5_ERR_RANKS;
    if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));

    int hand[5] = { -1, -1, -1, -1, -1 };
    double eq = -1, lo = 0, hi = 100;
    plo5_player pl[2];
    memset(pl, 0, sizeof pl);
    pl[0].type = PLO5_P_FIXED;
    pl[1].type = PLO5_P_RANDOM;
    for (int it = 0; it < 12; it++) {
        double mid = (lo + hi) * 0.5;
        int rc2 = dbl && nb >= 3
            ? plo5_percentile_hand_2b(mid, board, nb, board2, nb2, hand)
            : plo5_percentile_hand_on(mid, nb >= 3 ? board : NULL,
                                      nb >= 3 ? nb : 0, hand);
        if (rc2 != PLO5_OK) return json_error(sb, plo5_api_err_str(rc2));
        memcpy(pl[0].cards, hand, sizeof hand);
        plo5_result rr;
        rc2 = dbl
            ? plo5_equity_2b(pl, 2, board, nb, board2, nb2, NULL, 0,
                             200000, 0, 777u + (unsigned)it, g_ncpu, &rr)
            : plo5_equity2(pl, 2, board, nb, NULL, 0,
                           200000, 0, 777u + (unsigned)it, g_ncpu, &rr);
        if (rc2 != PLO5_OK) return json_error(sb, plo5_api_err_str(rc2));
        eq = rr.equity[0];
        if (g_prog) { g_prog_what = "searching"; g_prog(g_prog_what, it + 1, 12, g_prog_ud); }
        if (fabs(eq - target) < 0.004) break;
        if (eq < target) lo = mid; else hi = mid;
    }
    char hs[16];
    hand_str_sorted(hand, 5, hs);
    plo5_sb_printf(sb, "{\"hand\":\"%s\",\"equity\":%.2f}", hs, eq * 100.0);
    return 200;
}

static int api_random(const char *q, plo5_sb *sb)
{
    char ustr[128];
    plo5_api_qget(q, "used", ustr, sizeof ustr);
    int street = (int)qnum(q, "street", 3);
    int count = (int)qnum(q, "count", 1);
    if (street < 3 || street > 5 || count < 1 || count > 2)
        return json_error(sb, "bad street/count");
    int used[52];
    int nu = parse_cards(ustr, used, 52);
    if (nu < 0) return json_error(sb, "bad used list");

    uint64_t mask = 0;
    for (int i = 0; i < nu; i++) mask |= 1ull << used[i];
    int pool[52], n = 0;
    for (int cd = 0; cd < 52; cd++)
        if (!(mask >> cd & 1)) pool[n++] = cd;
    int need = street * count;
    if (n < need) return json_error(sb, "not enough cards");

    uint64_t s = plo5_api_seed();
    for (int i = 0; i < need; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;   /* xorshift64 */
        int j = i + (int)(s % (uint64_t)(n - i));
        int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    char b1[16], b2[16] = "";
    cards_str(pool, street, b1);
    if (count == 2) cards_str(pool + street, street, b2);
    plo5_sb_printf(sb, "{\"board\":\"%s\",\"board2\":\"%s\"}", b1, b2);
    return 200;
}

/* Deal a hero hand whose equity vs `opp` MDF-trimmed stack-off ranges
 * sits `offset` percentage points from the no-fold-equity threshold
 * SPR/(1+nway*SPR) (offset may be negative: below the threshold).
 * Binary-searches the board-conditional percentile ladder, like
 * /api/findeq but against the trimmed multiway ranges. */
static int api_sprquiz(const char *q, plo5_sb *sb)
{
    char bstr[64];
    plo5_api_qget(q, "board", bstr, sizeof bstr);
    int board[5];
    int nb = parse_cards(bstr, board, 5);
    if (nb < 3) return json_error(sb, "board (3-5 cards) required");
    double spr = qnum(q, "spr", 5);
    int nopp = (int)qnum(q, "opp", 1);
    double offset = qnum(q, "offset", 5) / 100.0;
    double pot = qnum(q, "pot", 10);
    uint64_t trials = (uint64_t)qnum(q, "trials", 60000);
    if (spr <= 0 || nopp < 1 || nopp > PLO5_MAX_PLAYERS - 1 || pot <= 0)
        return json_error(sb, "bad spr/opp/pot");

    int rc = build_single(board, nb);
    if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));

    int nway = nopp + 1;
    double eqn = plo5_spr_eq_needed(spr, nway);
    double mdf = plo5_spr_mdf(spr);
    double tlo, thi;
    plo5_mdf_trim_band(0, 100, mdf, &tlo, &thi);

    double target = eqn + offset;
    if (target < 0.03) target = 0.03;
    if (target > 0.95) target = 0.95;

    /* find the hero hand; the band floor may widen when nopp disjoint
     * tight-band hands cannot be dealt (kept across iterations). The
     * percentile->equity ladder is coarse near the extremes multiway,
     * so keep the best hand seen rather than the last midpoint. */
    int hand[5] = { -1, -1, -1, -1, -1 }, best[5];
    plo5_spr_row row;
    int widened = 0;
    double lo = 0, hi = 100, best_gap = 1e9;
    for (int it = 0; it < 12; it++) {
        double mid = (lo + hi) * 0.5;
        rc = plo5_percentile_hand_on(mid, board, nb, hand);
        if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));
        for (;;) {
            rc = plo5_spr_row_eval(hand, board, nb, tlo, thi, nopp, pot, spr,
                                   trials, 777u + (unsigned)it, g_ncpu, &row);
            if (rc != PLO5_ERR_RANGE || tlo <= 1e-9) break;
            tlo = tlo - 5.0 > 0 ? tlo - 5.0 : 0;
            widened = 1;
        }
        if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));
        double gap = fabs(row.hero_eq - target);
        if (gap < best_gap) { best_gap = gap; memcpy(best, hand, sizeof best); }
        if (g_prog) { g_prog_what = "searching"; g_prog(g_prog_what, it + 1, 12, g_prog_ud); }
        if (gap < 0.004) break;
        if (row.hero_eq < target) lo = mid; else hi = mid;
    }
    memcpy(hand, best, sizeof best);
    /* confirm the final hand with more trials (more trials = more chances
     * to hit an undealable rejection streak, so keep widening here too) */
    for (;;) {
        rc = plo5_spr_row_eval(hand, board, nb, tlo, thi, nopp, pot, spr,
                               250000, 424242u, g_ncpu, &row);
        if (rc != PLO5_ERR_RANGE || tlo <= 1e-9) break;
        tlo = tlo - 5.0 > 0 ? tlo - 5.0 : 0;
        widened = 1;
    }
    if (rc != PLO5_OK) return json_error(sb, plo5_api_err_str(rc));

    char hs[16], bs[16];
    hand_str_sorted(hand, 5, hs);
    cards_str(board, nb, bs);   /* canonical, never the raw query text */

    plo5_sb_printf(sb,
        "{\"hand\":\"%s\",\"board\":\"%s\",\"spr\":%.2f,\"opp\":%d,"
        "\"pot\":%.2f,\"stack\":%.2f,\"potFinal\":%.2f,"
        "\"equity\":%.2f,\"eqNeeded\":%.2f,\"mdf\":%.2f,"
        "\"bandLo\":%.1f,\"bandHi\":%.1f,\"bandWidened\":%s,",
        hs, bs, spr, nopp, pot, row.stack, row.pot_final,
        row.hero_eq * 100.0, row.eq_needed * 100.0, mdf * 100.0,
        tlo, thi, widened ? "true" : "false");
    if (row.profitable_no_fold)
        sb_lit(sb, "\"breakevenFold\":null}");
    else
        plo5_sb_printf(sb, "\"breakevenFold\":%.1f}", row.breakeven_fold * 100.0);
    return 200;
}

static int api_status(plo5_sb *sb)
{
    static const char *pnames[3] = { "Fast", "Balanced", "Precise" };
    plo5_sb_printf(sb, "{\"ranks\":%s,\"ncpu\":%d,\"categories\":[",
                   plo5_ranks_loaded() ? "true" : "false", g_ncpu);
    for (int i = 0; i < PLO5_NCAT; i++)
        plo5_sb_printf(sb, "%s\"%s\"", i ? "," : "", plo5_category_name(i));
    sb_lit(sb, "],\"presets\":[");
    for (int i = 0; i < 3; i++)
        plo5_sb_printf(sb, "%s{\"name\":\"%s\",\"trials\":%llu,\"maxenum\":%llu}",
                       i ? "," : "", pnames[i],
                       (unsigned long long)g_presets[i],
                       (unsigned long long)(g_presets[i] / 5));
    plo5_sb_put(sb, "]}", 2);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int plo5_api_call(const char *endpoint, const char *query, plo5_sb *out,
                  plo5_api_progress_fn progress, void *ud)
{
    if (!query) query = "";
    g_prog = progress;
    g_prog_ud = ud;
    int code;
    if      (!strcmp(endpoint, "/api/equity"))  code = api_equity(query, out);
    else if (!strcmp(endpoint, "/api/lookup"))  code = api_lookup(query, out);
    else if (!strcmp(endpoint, "/api/rankof"))  code = api_rankof(query, out);
    else if (!strcmp(endpoint, "/api/findeq"))  code = api_findeq(query, out);
    else if (!strcmp(endpoint, "/api/random"))  code = api_random(query, out);
    else if (!strcmp(endpoint, "/api/sprquiz")) code = api_sprquiz(query, out);
    else if (!strcmp(endpoint, "/api/status"))  code = api_status(out);
    else { code = 404; sb_lit(out, "{\"error\":\"no such api\"}"); }
    g_prog = NULL;
    g_prog_ud = NULL;
    return code;
}
