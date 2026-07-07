/* web.c — local web UI server for the PLO5 equity engine.
 *
 * Serves the React app in web/ (next to the exe) and exposes the engine
 * as a small JSON-over-HTTP API on 127.0.0.1 only. Single-threaded
 * request handling (the engine itself is multithreaded internally),
 * which also keeps the board-ranking caches race-free.
 *
 * Build (MSVC):  cl /O2 /std:c11 /utf-8 /Isrc src\web.c src\plo5.c /Fe:plo5web.exe ws2_32.lib shell32.lib
 * Build (gcc) :  gcc -O3 -std=c11 -Isrc -o plo5web src/web.c src/plo5.c -lws2_32
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plo5.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

static int g_ncpu = 1;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* growable string buffer */
typedef struct { char *buf; size_t len, cap; } sb_t;

static void sb_init(sb_t *s) { s->buf = NULL; s->len = s->cap = 0; }
static void sb_free(sb_t *s) { free(s->buf); sb_init(s); }

static void sb_put(sb_t *s, const char *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        while (nc < s->len + n + 1) nc *= 2;
        s->buf = realloc(s->buf, nc);
        s->cap = nc;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = 0;
}

static void sb_printf(sb_t *s, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(s, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

/* url-decode value of key from a query string; 0 if missing */
static int qget(const char *query, const char *key, char *out, size_t cap)
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
                    char hx[3] = { v[i + 1], v[i + 2], 0 };
                    c = (char)strtol(hx, NULL, 16);
                    i += 2;
                }
                out[o++] = c;
            }
            out[o] = 0;
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return 0;
}

static double qnum(const char *query, const char *key, double dflt)
{
    char v[64];
    if (!qget(query, key, v, sizeof v) || !v[0]) return dflt;
    return atof(v);
}

/* ------------------------------------------------------------------ */
/* Hand spec parsing (same syntax as the CLI)                          */
/* ------------------------------------------------------------------ */

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

static const char *err_str(int rc)
{
    switch (rc) {
    case PLO5_ERR_PLAYERS: return "need 2..6 players";
    case PLO5_ERR_BOARD:   return "board must have 0, 3, 4 or 5 cards";
    case PLO5_ERR_CARD:    return "invalid card";
    case PLO5_ERR_DUP:     return "duplicate card";
    case PLO5_ERR_DECK:    return "not enough cards left in deck";
    case PLO5_ERR_TRIALS:  return "no trials and enumeration not possible";
    case PLO5_ERR_RANKS:   return "rank table missing - run plo5calc --gen-ranks";
    case PLO5_ERR_IO:      return "cannot read/write rank file";
    case PLO5_ERR_RANGE:   return "range cannot be dealt with these dead cards";
    default:               return "invalid arguments";
    }
}

/* ------------------------------------------------------------------ */
/* HTTP plumbing                                                       */
/* ------------------------------------------------------------------ */

static void send_all(SOCKET c, const char *p, size_t n)
{
    while (n > 0) {
        int k = send(c, p, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
        if (k <= 0) return;
        p += k;
        n -= (size_t)k;
    }
}

static void respond(SOCKET c, int code, const char *ctype,
                    const char *body, size_t blen)
{
    char hdr[512];
    const char *st = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        code, st, ctype, blen);
    send_all(c, hdr, (size_t)hn);
    if (blen) send_all(c, body, blen);
}

static void respond_json(SOCKET c, int code, sb_t *sb)
{
    respond(c, code, "application/json; charset=utf-8", sb->buf ? sb->buf : "{}",
            sb->len);
    sb_free(sb);
}

static void json_error(SOCKET c, const char *msg)
{
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"error\":\"%s\"}", msg);
    respond_json(c, 400, &sb);
}

/* ------------------------------------------------------------------ */
/* Static files                                                        */
/* ------------------------------------------------------------------ */

static char g_webdir[MAX_PATH];

static const char *mime_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!_stricmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!_stricmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!_stricmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!_stricmp(dot, ".svg"))  return "image/svg+xml";
    if (!_stricmp(dot, ".ico"))  return "image/x-icon";
    if (!_stricmp(dot, ".png"))  return "image/png";
    if (!_stricmp(dot, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

static void serve_static(SOCKET c, const char *urlpath)
{
    if (strstr(urlpath, "..") || strchr(urlpath, '\\')) {
        respond(c, 400, "text/plain", "bad path", 8);
        return;
    }
    char full[MAX_PATH * 2];
    snprintf(full, sizeof full, "%s%s%s", g_webdir,
             urlpath[0] == '/' ? "" : "/",
             strcmp(urlpath, "/") == 0 ? "/index.html" : urlpath);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';

    FILE *f = fopen(full, "rb");
    if (!f) {
        respond(c, 404, "text/plain", "not found", 9);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        respond(c, 404, "text/plain", "read error", 10);
        return;
    }
    fclose(f);
    respond(c, 200, mime_of(full), buf, (size_t)n);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */

#define sb_lit(s, lit) sb_put(s, lit, strlen(lit))

static void put_dbl_array(sb_t *sb, const double *v, int n, double scale)
{
    sb_lit(sb, "[");
    for (int i = 0; i < n; i++)
        sb_printf(sb, "%s%.6f", i ? "," : "", v[i] * scale);
    sb_lit(sb, "]");
}

/* GET /api/equity?players=SPEC,SPEC[,..]&board=..&board2=..&double=0/1
 *   &dead=..&trials=N&maxenum=N&seed=N&buildranks=0/1 */
static void api_equity(SOCKET c, const char *q)
{
    char pspec[512], bstr[64], b2str[64], dstr[64];
    if (!qget(q, "players", pspec, sizeof pspec) || !pspec[0]) {
        json_error(c, "players parameter required");
        return;
    }
    qget(q, "board", bstr, sizeof bstr);
    qget(q, "board2", b2str, sizeof b2str);
    qget(q, "dead", dstr, sizeof dstr);

    plo5_player pl[PLO5_MAX_PLAYERS];
    int np = 0;
    char *tok, *ctx = NULL;
    for (tok = strtok_s(pspec, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        if (np >= PLO5_MAX_PLAYERS) { json_error(c, "too many players"); return; }
        if (parse_player(tok, &pl[np]) != 0) {
            json_error(c, "cannot parse a player spec");
            return;
        }
        np++;
    }

    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    int dead[20];
    int nd = parse_cards(dstr, dead, 20);
    if (nb < 0 || nb2 < 0 || nd < 0) { json_error(c, "bad card list"); return; }

    int dbl = (int)qnum(q, "double", 0) || nb2 > 0;
    uint64_t trials = (uint64_t)qnum(q, "trials", 1000000);
    uint64_t maxen = (uint64_t)qnum(q, "maxenum", 1000000);
    uint64_t seed = (uint64_t)qnum(q, "seed", 0);
    if (!seed) {
        LARGE_INTEGER qc;
        QueryPerformanceCounter(&qc);
        seed = (uint64_t)qc.QuadPart;
    }

    /* optionally (re)build board rankings so percentiles come back filled */
    if ((int)qnum(q, "buildranks", 0)) {
        if (dbl && nb >= 3 && nb2 >= 3)
            plo5_board_ranks_build2(board, nb, board2, nb2, 100, g_ncpu, NULL, NULL);
        else if (!dbl && nb >= 3)
            plo5_board_ranks_build(board, nb, 100, g_ncpu, NULL, NULL);
    }

    plo5_result r;
    double t0 = now_ms();
    int rc = dbl
        ? plo5_equity_2b(pl, np, board, nb, board2, nb2, dead, nd,
                         trials, maxen, seed, g_ncpu, &r)
        : plo5_equity2(pl, np, board, nb, dead, nd,
                       trials, maxen, seed, g_ncpu, &r);
    double ms = now_ms() - t0;
    if (rc != PLO5_OK) { json_error(c, err_str(rc)); return; }

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"samples\":%llu,\"exact\":%s,\"ms\":%.1f,\"double\":%s,",
              (unsigned long long)r.samples, r.exact ? "true" : "false",
              ms, dbl ? "true" : "false");
    sb_lit(&sb, "\"equity\":"); put_dbl_array(&sb, r.equity, np, 100);
    sb_lit(&sb, ",\"win\":");   put_dbl_array(&sb, r.win, np, 100);
    sb_lit(&sb, ",\"tie\":");   put_dbl_array(&sb, r.tie, np, 100);
    sb_lit(&sb, ",\"ci95\":");  put_dbl_array(&sb, r.ci95, np, 100);

    sb_lit(&sb, ",\"handType\":[");
    for (int p = 0; p < np; p++) {
        if (p) sb_lit(&sb, ",");
        put_dbl_array(&sb, r.hand_type[p], PLO5_NCAT, 100);
    }
    sb_lit(&sb, "]");
    if (dbl) {
        sb_lit(&sb, ",\"handTypeB\":[");
        for (int p = 0; p < np; p++) {
            if (p) sb_lit(&sb, ",");
            put_dbl_array(&sb, r.hand_type_b[p], PLO5_NCAT, 100);
        }
        sb_lit(&sb, "]");
    }

    /* percentile of each fixed hand, when the matching table is available */
    sb_lit(&sb, ",\"pct\":[");
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
        if (pc >= 0) sb_printf(&sb, "%s%.1f", p ? "," : "", pc);
        else sb_printf(&sb, "%snull", p ? "," : "");
    }
    sb_put(&sb, "]}", 2);
    respond_json(c, 200, &sb);
}

/* GET /api/lookup?pct=30&board=..&board2=.. -> hand at that percentile */
static void api_lookup(SOCKET c, const char *q)
{
    char bstr[64], b2str[64];
    qget(q, "board", bstr, sizeof bstr);
    qget(q, "board2", b2str, sizeof b2str);
    double pct = qnum(q, "pct", 50);
    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) { json_error(c, "bad card list"); return; }

    int h[5], rc;
    const char *street;
    if (nb2 >= 3 && nb >= 3) {
        rc = plo5_board_ranks_build2(board, nb, board2, nb2, 100, g_ncpu, NULL, NULL);
        if (rc == PLO5_OK)
            rc = plo5_percentile_hand_2b(pct, board, nb, board2, nb2, h);
        street = "both boards";
    } else if (nb >= 3) {
        rc = plo5_board_ranks_build(board, nb, 100, g_ncpu, NULL, NULL);
        if (rc == PLO5_OK)
            rc = plo5_percentile_hand_on(pct, board, nb, h);
        street = nb == 3 ? "flop" : nb == 4 ? "turn" : "river";
    } else {
        rc = plo5_percentile_hand(pct, h);
        street = "preflop";
    }
    if (rc != PLO5_OK) { json_error(c, err_str(rc)); return; }

    /* sort descending for display */
    for (int i = 1; i < 5; i++) {
        int v = h[i], j = i - 1;
        while (j >= 0 && h[j] < v) { h[j + 1] = h[j]; j--; }
        h[j + 1] = v;
    }
    char hs[16] = "", cs[3];
    for (int i = 0; i < 5; i++) { plo5_card_str(h[i], cs); strcat(hs, cs); }
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"hand\":\"%s\",\"street\":\"%s\"}", hs, street);
    respond_json(c, 200, &sb);
}

/* GET /api/rankof?hand=..&board=..&board2=.. -> percentile of a hand */
static void api_rankof(SOCKET c, const char *q)
{
    char hstr[32], bstr[64], b2str[64];
    if (!qget(q, "hand", hstr, sizeof hstr)) { json_error(c, "hand required"); return; }
    qget(q, "board", bstr, sizeof bstr);
    qget(q, "board2", b2str, sizeof b2str);
    int h[5], board[5], board2[5];
    if (parse_cards(hstr, h, 5) != 5) { json_error(c, "bad hand"); return; }
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) { json_error(c, "bad card list"); return; }

    double pc;
    if (nb >= 3 && nb2 >= 3) {
        plo5_board_ranks_build2(board, nb, board2, nb2, 100, g_ncpu, NULL, NULL);
        pc = plo5_hand_percentile_2b(h, board, nb, board2, nb2);
    } else if (nb >= 3) {
        plo5_board_ranks_build(board, nb, 100, g_ncpu, NULL, NULL);
        pc = plo5_hand_percentile_on(h, board, nb);
    } else {
        pc = plo5_hand_percentile(h);
    }
    if (pc < 0) { json_error(c, "percentile unavailable (table missing or card conflict)"); return; }
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"pct\":%.2f}", pc);
    respond_json(c, 200, &sb);
}

/* GET /api/findeq?target=55&board=..&board2=..&double=0/1
 * binary-search the strength distribution for a hand with the target
 * equity vs one random opponent (same algorithm as the desktop GUI) */
static void api_findeq(SOCKET c, const char *q)
{
    char bstr[64], b2str[64];
    qget(q, "board", bstr, sizeof bstr);
    qget(q, "board2", b2str, sizeof b2str);
    double target = qnum(q, "target", 55) / 100.0;
    int board[5], board2[5];
    int nb = parse_cards(bstr, board, 5);
    int nb2 = parse_cards(b2str, board2, 5);
    if (nb < 0 || nb2 < 0) { json_error(c, "bad card list"); return; }
    int dbl = (int)qnum(q, "double", 0) || nb2 > 0;

    int rc = PLO5_OK;
    if (dbl && nb >= 3 && nb2 >= 3)
        rc = plo5_board_ranks_build2(board, nb, board2, nb2, 100, g_ncpu, NULL, NULL);
    else if (!dbl && nb >= 3)
        rc = plo5_board_ranks_build(board, nb, 100, g_ncpu, NULL, NULL);
    else if (!plo5_ranks_loaded())
        rc = PLO5_ERR_RANKS;
    if (rc != PLO5_OK) { json_error(c, err_str(rc)); return; }

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
        if (rc2 != PLO5_OK) { json_error(c, err_str(rc2)); return; }
        memcpy(pl[0].cards, hand, sizeof hand);
        plo5_result rr;
        rc2 = dbl
            ? plo5_equity_2b(pl, 2, board, nb, board2, nb2, NULL, 0,
                             200000, 0, 777u + (unsigned)it, g_ncpu, &rr)
            : plo5_equity2(pl, 2, board, nb, NULL, 0,
                           200000, 0, 777u + (unsigned)it, g_ncpu, &rr);
        if (rc2 != PLO5_OK) { json_error(c, err_str(rc2)); return; }
        eq = rr.equity[0];
        if (fabs(eq - target) < 0.004) break;
        if (eq < target) lo = mid; else hi = mid;
    }
    for (int i = 1; i < 5; i++) {
        int v = hand[i], j = i - 1;
        while (j >= 0 && hand[j] < v) { hand[j + 1] = hand[j]; j--; }
        hand[j + 1] = v;
    }
    char hs[16] = "", cs[3];
    for (int i = 0; i < 5; i++) { plo5_card_str(hand[i], cs); strcat(hs, cs); }
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"hand\":\"%s\",\"equity\":%.2f}", hs, eq * 100.0);
    respond_json(c, 200, &sb);
}

/* GET /api/random?street=3&count=1/2&used=... -> random board(s) from
 * cards not in `used` */
static void api_random(SOCKET c, const char *q)
{
    char ustr[128];
    qget(q, "used", ustr, sizeof ustr);
    int street = (int)qnum(q, "street", 3);
    int count = (int)qnum(q, "count", 1);
    if (street < 3 || street > 5 || count < 1 || count > 2) {
        json_error(c, "bad street/count");
        return;
    }
    int used[40];
    int nu = parse_cards(ustr, used, 40);
    if (nu < 0) { json_error(c, "bad used list"); return; }

    uint64_t mask = 0;
    for (int i = 0; i < nu; i++) mask |= 1ull << used[i];
    int pool[52], n = 0;
    for (int cd = 0; cd < 52; cd++)
        if (!(mask >> cd & 1)) pool[n++] = cd;
    int need = street * count;
    if (n < need) { json_error(c, "not enough cards"); return; }

    LARGE_INTEGER qc;
    QueryPerformanceCounter(&qc);
    uint64_t s = (uint64_t)qc.QuadPart;
    for (int i = 0; i < need; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;   /* xorshift64 */
        int j = i + (int)(s % (uint64_t)(n - i));
        int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    char b1[16] = "", b2[16] = "", cs[3];
    for (int i = 0; i < street; i++) { plo5_card_str(pool[i], cs); strcat(b1, cs); }
    if (count == 2)
        for (int i = 0; i < street; i++) { plo5_card_str(pool[street + i], cs); strcat(b2, cs); }
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"board\":\"%s\",\"board2\":\"%s\"}", b1, b2);
    respond_json(c, 200, &sb);
}

static void api_status(SOCKET c)
{
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"ranks\":%s,\"ncpu\":%d,\"categories\":[",
              plo5_ranks_loaded() ? "true" : "false", g_ncpu);
    for (int i = 0; i < PLO5_NCAT; i++)
        sb_printf(&sb, "%s\"%s\"", i ? "," : "", plo5_category_name(i));
    sb_put(&sb, "]}", 2);
    respond_json(c, 200, &sb);
}

/* ------------------------------------------------------------------ */
/* Server loop                                                         */
/* ------------------------------------------------------------------ */

/* Socket I/O runs on one thread per connection (browsers open idle
 * speculative connections that must not block the server), but all
 * engine work is serialized — the board-ranking caches assume single-
 * threaded mutation. */
static CRITICAL_SECTION g_engine_cs;

static void handle(SOCKET c)
{
    char req[8192];
    int got = 0;
    while (got < (int)sizeof req - 1) {
        int k = recv(c, req + got, (int)sizeof req - 1 - got, 0);
        if (k <= 0) break;
        got += k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (got <= 0) return;

    char method[8], url[2048];
    if (sscanf(req, "%7s %2047s", method, url) != 2) return;
    if (strcmp(method, "GET") != 0) {
        respond(c, 400, "text/plain", "GET only", 8);
        return;
    }
    char *qs = strchr(url, '?');
    if (qs) *qs++ = 0;
    else qs = url + strlen(url);

    if (strncmp(url, "/api/", 5) == 0) {
        EnterCriticalSection(&g_engine_cs);
        if (strcmp(url, "/api/equity") == 0)       api_equity(c, qs);
        else if (strcmp(url, "/api/lookup") == 0)  api_lookup(c, qs);
        else if (strcmp(url, "/api/rankof") == 0)  api_rankof(c, qs);
        else if (strcmp(url, "/api/findeq") == 0)  api_findeq(c, qs);
        else if (strcmp(url, "/api/random") == 0)  api_random(c, qs);
        else if (strcmp(url, "/api/status") == 0)  api_status(c);
        else respond(c, 404, "text/plain", "no such api", 11);
        LeaveCriticalSection(&g_engine_cs);
    } else {
        serve_static(c, url);
    }
}

static DWORD WINAPI conn_thread(LPVOID vp)
{
    SOCKET c = (SOCKET)(uintptr_t)vp;
    DWORD tmo = 10000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    tmo = 30000;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
    handle(c);
    shutdown(c, SD_SEND);
    closesocket(c);
    return 0;
}

int main(int argc, char **argv)
{
    plo5_init();

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_ncpu = (int)si.dwNumberOfProcessors;
    if (g_ncpu < 1) g_ncpu = 1;

    /* web dir + rank table live next to the exe */
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(g_webdir, sizeof g_webdir, "%s\\web", exe);

    char rankpath[MAX_PATH];
    snprintf(rankpath, sizeof rankpath, "%s\\plo5rank.bin", exe);
    if (plo5_ranks_load(rankpath) == PLO5_OK)
        printf("preflop rank table loaded\n");
    else
        printf("note: plo5rank.bin not found - preflop percentiles disabled\n"
               "      (build it once with: plo5calc --gen-ranks --threads 0)\n");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */

    int open_browser = 1, port = 8722;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-browser") == 0) open_browser = 0;
        else if (atoi(argv[i]) > 0) port = atoi(argv[i]);
    }
    int bound = 0;
    for (int tryp = port; tryp < port + 10; tryp++) {
        addr.sin_port = htons((u_short)tryp);
        if (bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0) {
            port = tryp;
            bound = 1;
            break;
        }
    }
    if (!bound || listen(srv, 8) != 0) {
        fprintf(stderr, "error: cannot bind a local port\n");
        return 1;
    }

    char urlbuf[64];
    snprintf(urlbuf, sizeof urlbuf, "http://127.0.0.1:%d", port);
    printf("PLO5 web UI running at %s  (Ctrl+C to stop)\n", urlbuf);
    if (open_browser)
        ShellExecuteA(NULL, "open", urlbuf, NULL, NULL, SW_SHOWNORMAL);

    InitializeCriticalSection(&g_engine_cs);
    for (;;) {
        SOCKET cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) continue;
        HANDLE th = CreateThread(NULL, 0, conn_thread,
                                 (LPVOID)(uintptr_t)cli, 0, NULL);
        if (th) CloseHandle(th);
        else conn_thread((LPVOID)(uintptr_t)cli);
    }
}
