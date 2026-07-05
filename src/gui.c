/* gui.c — native Win32 GUI for the PLO5 equity calculator (v2).
 *
 * Adds percentile-range players (dual-handle sliders backed by the
 * plo5rank.bin hand-strength table), a percentile lookup box, and a
 * Calculate button with optional Auto recalculation.
 *
 * Build (MSVC):  cl /O2 /std:c11 /utf-8 gui.c plo5.c /Fe:plo5gui.exe /link /subsystem:windows
 * Build (gcc) :  gcc -O3 -std=c11 -mwindows -o plo5gui gui.c plo5.c -lgdi32
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "plo5.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

#define NP_MAX      6
#define SLOT_BOARD  (NP_MAX * 5)        /* 30..34 */
#define SLOT_DEAD   (SLOT_BOARD + 5)    /* 35..42 */
#define NDEAD       8
#define SLOT_BOARD2 (SLOT_DEAD + NDEAD) /* 43..47, double-board mode */
#define NSLOTS      (SLOT_BOARD2 + 5)
#define NDEAD_DB    4                   /* dead slots shown in double mode */

static int g_db = 0;                    /* double-board split pot */

static int    slot_card[NSLOTS];        /* card id or -1 */
static int    card_slot[52];            /* owning slot or -1 */
static int    g_active = 0;             /* selected slot, -1 = none */
static int    g_np = 2;                 /* players shown, 2..6 */
static int    g_mode[NP_MAX];           /* PLO5_P_FIXED / _RANGE / _RANDOM */
static double g_lo[NP_MAX], g_hi[NP_MAX];
static int    g_sel_p = 0;              /* target player for Deal */
static int    g_prec = 1;
static int    g_auto = 0;               /* auto-recalculate off by default */
static int    g_stale = 1;
static int    g_ranks = 0;              /* rank table loaded */
static int    g_pend_rank = -1;
static double g_qpct = 30.0;            /* percentile query */
static int    g_qhand[5] = { -1, -1, -1, -1, -1 };
static double g_eq_target = 55.0;       /* equity-finder target, percent */
static int    g_ehand[5] = { -1, -1, -1, -1, -1 };
static double g_ehand_eq = -1;          /* measured equity of found hand */

/* splitmix64 for random-board dealing */
static uint64_t g_rng;
static uint64_t rnd64(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static int rnd_below(int n) { return (int)(((rnd64() >> 32) * (uint64_t)n) >> 32); }

static const struct { const wchar_t *name; uint64_t trials, max_enum; } PREC[3] = {
    { L"Fast",     200000,   20000 },
    { L"Balanced", 1000000,  200000 },
    { L"Precise",  10000000, 2000000 },
};

/* results shown in the UI */
static plo5_result g_res;
static int      g_res_ok = 0;
static int      g_res_np = 0;
static double   g_res_ms = 0;
static unsigned g_gen = 0, g_done_gen = 0;
static wchar_t  g_hint[192];            /* why the state is not computable */
static wchar_t  g_err[128];             /* last engine error */

typedef struct {
    plo5_player pl[NP_MAX];
    int np;
    int board[5], nb;
    int board2[5], nb2;
    int db;                     /* double board split pot */
    int dead[NDEAD], nd;
    uint64_t trials, max_enum;
    int build_only;             /* just build the board ranking */
    int find_eq;                /* search for a hand with this equity */
    double target;              /* target equity, 0..1 */
    unsigned gen;
} req_t;

static req_t            g_req;
static CRITICAL_SECTION g_cs;
static HANDLE           g_evt;
static plo5_result      g_wres;
static double           g_wms;
static int              g_wrc;
static int              g_wehand[5];    /* equity-finder result */
static double           g_weq;
static int              g_ncpu = 1;

#define WM_APP_RESULT (WM_APP + 1)

/* ------------------------------------------------------------------ */
/* Look                                                                */
/* ------------------------------------------------------------------ */

#define C_BG       RGB(243, 244, 246)
#define C_CARD     RGB(255, 255, 255)
#define C_CARD_DIM RGB(228, 229, 233)
#define C_TXT_DIM  RGB(178, 181, 188)
#define C_BORDER   RGB(203, 206, 212)
#define C_HOT      RGB(226, 237, 252)
#define C_SEL      RGB(26, 115, 232)
#define C_SEL_BG   RGB(232, 240, 254)
#define C_TXT      RGB(32, 33, 36)
#define C_TXT_SOFT RGB(95, 99, 104)
#define C_BAR_BG   RGB(224, 226, 230)
#define C_EMPTY    RGB(248, 249, 250)
#define C_GOOD     RGB(24, 128, 56)
#define C_WARN     RGB(200, 108, 0)

static const COLORREF PCOL[NP_MAX] = {
    RGB(26, 115, 232), RGB(217, 48, 37), RGB(24, 128, 56),
    RGB(242, 153, 0),  RGB(146, 64, 192), RGB(0, 151, 167)
};

static COLORREF mixc(COLORREF a, COLORREF b, int pct_b)
{
    return RGB((GetRValue(a) * (100 - pct_b) + GetRValue(b) * pct_b) / 100,
               (GetGValue(a) * (100 - pct_b) + GetGValue(b) * pct_b) / 100,
               (GetBValue(a) * (100 - pct_b) + GetBValue(b) * pct_b) / 100);
}

static COLORREF suit_col(int s)
{
    switch (s) {
    case 0:  return RGB(24, 128, 56);
    case 1:  return RGB(26, 115, 232);
    case 2:  return RGB(217, 48, 37);
    default: return RGB(32, 33, 36);
    }
}

static const wchar_t RANKW[] = L"23456789TJQKA";
static const wchar_t SUITW[5] = { 0x2663, 0x2666, 0x2665, 0x2660, 0 };

static HFONT g_f_ui, g_f_sm, g_f_card, g_f_big, g_f_lbl, g_f_calc;
static int   g_dpi = 96;
static HWND  g_hwnd;
static HWND  g_ed_lo[NP_MAX], g_ed_hi[NP_MAX], g_ed_pct, g_ed_eq;
static int   g_syncing = 0;             /* suppress EN_CHANGE feedback */

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void make_fonts(void)
{
    if (g_f_ui) {
        DeleteObject(g_f_ui); DeleteObject(g_f_sm); DeleteObject(g_f_card);
        DeleteObject(g_f_big); DeleteObject(g_f_lbl); DeleteObject(g_f_calc);
    }
    #define MKF(px, w) CreateFontW(-S(px), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, \
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, \
        DEFAULT_PITCH, L"Segoe UI")
    g_f_ui   = MKF(14, FW_NORMAL);
    g_f_sm   = MKF(12, FW_NORMAL);
    g_f_card = MKF(15, FW_SEMIBOLD);
    g_f_big  = MKF(20, FW_SEMIBOLD);
    g_f_lbl  = MKF(12, FW_SEMIBOLD);
    g_f_calc = MKF(16, FW_SEMIBOLD);
    #undef MKF
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

enum {
    B_PMINUS = 1, B_PPLUS, B_PREC, B_CLEARALL, B_COPY, B_CALC, B_AUTO, B_DEAL,
    B_RANKB, B_SB, B_DBL, B_QUIZ, B_RFLOP, B_RTURN, B_RRIVER, B_FINDEQ,
    B_MODE0 = 40,   /* + p*4 + m   (m: 0 fixed, 1 range, 2 random) */
    B_PCLR0 = 70    /* + p */
};

typedef struct { RECT r; int id; wchar_t label[16]; int on; int dis; } btn_t;

static RECT  g_card_r[52];
static RECT  g_slot_r[NSLOTS];
static btn_t g_btn[40];
static int   g_nbtn = 0;
static RECT  g_status_r, g_qhand_r, g_bottom_r, g_rndlbl_r, g_ehand_r;
static RECT  g_track_r[NP_MAX];         /* slider track (range mode) */
static RECT  g_prow_r[NP_MAX];
static int   g_cw, g_ch;
static int   g_hot_card = -1, g_hot_slot = -1, g_hot_btn = -1;
static int   g_drag_p = -1, g_drag_hi = 0;

static int cur_board(int out[5]);
static int cur_board2(int out[5]);

static void add_btn(int id, int x, int y, int w, int h, const wchar_t *label,
                    int on, int dis)
{
    btn_t *b = &g_btn[g_nbtn++];
    b->r.left = x; b->r.top = y; b->r.right = x + w; b->r.bottom = y + h;
    b->id = id; b->on = on; b->dis = dis;
    lstrcpynW(b->label, label, 16);
}

static void compute_layout(void)
{
    const int M = S(14), CW = S(34), CH = S(44), G = S(5);
    g_nbtn = 0;
    g_cw = S(886);

    /* deck grid: rows = spades, hearts, diamonds, clubs; cols A..2 */
    static const int SUIT_ROW[4] = { 3, 2, 1, 0 };
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 13; col++) {
            int card = (12 - col) * 4 + SUIT_ROW[row];
            RECT *r = &g_card_r[card];
            r->left = M + col * (CW + G);
            r->top = M + row * (CH + G);
            r->right = r->left + CW;
            r->bottom = r->top + CH;
        }
    int grid_h = 4 * CH + 3 * G;

    /* side panel — the game-mode toggle sits on top, full width */
    int px = S(532), pr = g_cw - M;
    add_btn(B_SB, px, S(14), S(150), S(30), L"Single board", !g_db, 0);
    add_btn(B_DBL, px + S(158), S(14), S(182), S(30), L"Double board · split",
            g_db, 0);
    add_btn(B_PMINUS, px + S(68), S(52), S(26), S(26), L"−", 0, 0);
    add_btn(B_PPLUS, px + S(136), S(52), S(26), S(26), L"+", 0, 0);
    add_btn(B_QUIZ, pr - S(144), S(52), S(68), S(26), L"Quiz", 0, 0);
    add_btn(B_COPY, pr - S(68), S(52), S(68), S(26), L"Copy", 0, 0);
    add_btn(B_PREC, px + S(68), S(86), S(104), S(26), PREC[g_prec].name, 0, 0);
    add_btn(B_CLEARALL, pr - S(84), S(86), S(84), S(26), L"Clear all", 0, 0);
    add_btn(B_CALC, px, S(120), S(160), S(36), L"Calculate", 0, 0);
    add_btn(B_AUTO, px + S(168), S(120), S(60), S(36), L"Auto", g_auto, 0);
    add_btn(B_RANKB, px + S(236), S(120), S(104), S(36), L"Rank board", 0, 0);
    add_btn(B_DEAL, pr - S(76), S(164), S(76), S(24), L"", 0, 0);
    g_qhand_r.left = px + S(120); g_qhand_r.top = S(164);
    g_qhand_r.right = pr - S(84); g_qhand_r.bottom = S(188);
    add_btn(B_FINDEQ, px + S(120), S(198), S(52), S(24), L"Find", 0, 0);
    g_ehand_r.left = px + S(180); g_ehand_r.top = S(198);
    g_ehand_r.right = px + S(268); g_ehand_r.bottom = S(222);
    g_status_r.left = px; g_status_r.top = S(230);

    /* board + dead row (below both the grid and the taller panel) */
    int by = M + grid_h + S(86);
    g_status_r.right = pr; g_status_r.bottom = by - S(6);
    if (!g_db) {
        for (int i = 0; i < 5; i++) {
            RECT *r = &g_slot_r[SLOT_BOARD + i];
            r->left = S(60) + i * (CW + G);
            r->top = by;
            r->right = r->left + CW; r->bottom = r->top + CH;
        }
        for (int i = 0; i < NDEAD; i++) {
            RECT *r = &g_slot_r[SLOT_DEAD + i];
            r->left = S(316) + i * (CW + G);
            r->top = by;
            r->right = r->left + CW; r->bottom = r->top + CH;
        }
    } else {
        for (int i = 0; i < 5; i++) {
            RECT *r = &g_slot_r[SLOT_BOARD + i];
            r->left = S(60) + i * (CW + G);
            r->top = by;
            r->right = r->left + CW; r->bottom = r->top + CH;
            RECT *r2 = &g_slot_r[SLOT_BOARD2 + i];
            r2->left = S(310) + i * (CW + G);
            r2->top = by;
            r2->right = r2->left + CW; r2->bottom = r2->top + CH;
        }
        for (int i = 0; i < NDEAD_DB; i++) {
            RECT *r = &g_slot_r[SLOT_DEAD + i];
            r->left = S(560) + i * (CW + G);
            r->top = by;
            r->right = r->left + CW; r->bottom = r->top + CH;
        }
    }

    /* random-board buttons at the right end of the board row */
    {
        int rx = g_db ? S(716) : S(660);
        int rw = g_db ? S(48) : S(56);
        int rg = g_db ? S(4) : S(6);
        add_btn(B_RFLOP, rx, by + S(18), rw, S(26), L"Flop", 0, 0);
        add_btn(B_RTURN, rx + rw + rg, by + S(18), rw, S(26), L"Turn", 0, 0);
        add_btn(B_RRIVER, rx + 2 * (rw + rg), by + S(18), rw, S(26),
                L"River", 0, 0);
        g_rndlbl_r.left = rx;
        g_rndlbl_r.right = rx + 3 * rw + 2 * rg;
        g_rndlbl_r.top = by;
        g_rndlbl_r.bottom = by + S(16);
    }

    /* player rows */
    int y0 = by + CH + S(14), RH = S(58);
    for (int p = 0; p < g_np; p++) {
        int y = y0 + p * RH;
        g_prow_r[p].left = M; g_prow_r[p].top = y;
        g_prow_r[p].right = g_cw - M; g_prow_r[p].bottom = y + RH;

        static const wchar_t *MODES[3] = { L"Hand", L"Range", L"Rnd" };
        int bb[5], bb2[5];
        int range_dis = !g_ranks &&
            (g_db ? (cur_board(bb) < 3 || cur_board2(bb2) < 3)
                  : cur_board(bb) < 3);
        for (int m = 0; m < 3; m++)
            add_btn(B_MODE0 + p * 4 + m, S(46) + m * S(40), y + S(17),
                    S(40), S(24), MODES[m],
                    g_mode[p] == (m == 0 ? PLO5_P_FIXED :
                                  m == 1 ? PLO5_P_RANGE : PLO5_P_RANDOM),
                    m == 1 && range_dis);

        for (int i = 0; i < 5; i++) {
            RECT *r = &g_slot_r[p * 5 + i];
            r->left = S(174) + i * (CW + G);
            r->top = y + S(7);
            r->right = r->left + CW; r->bottom = r->top + CH;
        }
        g_track_r[p].left = S(174); g_track_r[p].right = S(404);
        g_track_r[p].top = y + S(36); g_track_r[p].bottom = y + S(44);

        add_btn(B_PCLR0 + p, S(420), y + S(17), S(22), S(24), L"×", 0, 0);
    }

    g_bottom_r.left = M;
    g_bottom_r.top = y0 + g_np * RH + S(6);
    g_bottom_r.right = g_cw - M;
    g_bottom_r.bottom = g_bottom_r.top + S(20);
    g_ch = g_bottom_r.bottom + S(10);

    /* Deal button label shows its target */
    for (int i = 0; i < g_nbtn; i++)
        if (g_btn[i].id == B_DEAL)
            swprintf(g_btn[i].label, 16, L"Deal P%d", g_sel_p + 1);
}

static void apply_layout(void)
{
    compute_layout();
    if (!g_hwnd) return;
    int y0;
    for (int p = 0; p < NP_MAX; p++) {
        int show = p < g_np && g_mode[p] == PLO5_P_RANGE;
        y0 = g_prow_r[p < g_np ? p : 0].top;
        if (p < g_np) {
            MoveWindow(g_ed_lo[p], S(174), y0 + S(6), S(44), S(24), TRUE);
            MoveWindow(g_ed_hi[p], S(238), y0 + S(6), S(44), S(24), TRUE);
        }
        ShowWindow(g_ed_lo[p], show ? SW_SHOW : SW_HIDE);
        ShowWindow(g_ed_hi[p], show ? SW_SHOW : SW_HIDE);
    }
    MoveWindow(g_ed_pct, S(532) + S(68), S(164), S(44), S(24), TRUE);
    MoveWindow(g_ed_eq, S(532) + S(68), S(198), S(44), S(24), TRUE);
}

static void resize_window(void)
{
    apply_layout();
    RECT r = { 0, 0, g_cw, g_ch };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                     WS_MINIMIZEBOX, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Compute                                                             */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_evt, INFINITE);
        EnterCriticalSection(&g_cs);
        req_t rq = g_req;
        LeaveCriticalSection(&g_cs);

        plo5_result r;
        memset(&r, 0, sizeof r);
        double t0 = now_ms();
        int rc;
        if (rq.find_eq) {
            /* binary search the strength distribution for a hand whose
             * equity vs one random opponent matches the target */
            int hand[5] = { -1, -1, -1, -1, -1 };
            double eq = -1;
            rc = PLO5_OK;
            if (rq.db && rq.nb >= 3 && rq.nb2 >= 3)
                rc = plo5_board_ranks_build2(rq.board, rq.nb, rq.board2,
                                             rq.nb2, 100, g_ncpu, NULL, NULL);
            else if (!rq.db && rq.nb >= 3)
                rc = plo5_board_ranks_build(rq.board, rq.nb, 100, g_ncpu,
                                            NULL, NULL);
            else if (!plo5_ranks_loaded())
                rc = PLO5_ERR_RANKS;
            if (rc == PLO5_OK) {
                double lo = 0, hi = 100;
                plo5_player pl[2];
                memset(pl, 0, sizeof pl);
                pl[0].type = PLO5_P_FIXED;
                pl[1].type = PLO5_P_RANDOM;
                for (int it = 0; it < 12; it++) {
                    double mid = (lo + hi) * 0.5;
                    int rc2 = rq.db && rq.nb >= 3
                        ? plo5_percentile_hand_2b(mid, rq.board, rq.nb,
                                                  rq.board2, rq.nb2, hand)
                        : plo5_percentile_hand_on(mid,
                              rq.nb >= 3 ? rq.board : NULL,
                              rq.nb >= 3 ? rq.nb : 0, hand);
                    if (rc2 != PLO5_OK) { rc = rc2; break; }
                    memcpy(pl[0].cards, hand, sizeof hand);
                    plo5_result rr;
                    rc2 = rq.db
                        ? plo5_equity_2b(pl, 2, rq.board, rq.nb, rq.board2,
                                         rq.nb2, NULL, 0, 200000, 0,
                                         777u + (unsigned)it, g_ncpu, &rr)
                        : plo5_equity2(pl, 2, rq.board, rq.nb, NULL, 0,
                                       200000, 0, 777u + (unsigned)it,
                                       g_ncpu, &rr);
                    if (rc2 != PLO5_OK) { rc = rc2; break; }
                    eq = rr.equity[0];
                    if (fabs(eq - rq.target) < 0.004) break;
                    if (eq < rq.target) lo = mid; else hi = mid;
                }
            }
            EnterCriticalSection(&g_cs);
            if (rq.gen == g_gen) {
                memcpy(g_wehand, hand, sizeof hand);
                g_weq = eq;
                g_wrc = rc;
            }
            LeaveCriticalSection(&g_cs);
            PostMessage(g_hwnd, WM_APP_RESULT, (WPARAM)rq.gen, (LPARAM)rc);
            continue;
        }
        if (rq.build_only)
            rc = rq.db
                ? plo5_board_ranks_build2(rq.board, rq.nb, rq.board2, rq.nb2,
                                          100, g_ncpu, NULL, NULL)
                : plo5_board_ranks_build(rq.board, rq.nb, 100, g_ncpu,
                                         NULL, NULL);
        else if (rq.db)
            rc = plo5_equity_2b(rq.pl, rq.np, rq.board, rq.nb,
                                rq.board2, rq.nb2, rq.dead, rq.nd,
                                rq.trials, rq.max_enum, 0xC0FFEE42ull,
                                g_ncpu, &r);
        else
            rc = plo5_equity2(rq.pl, rq.np, rq.board, rq.nb, rq.dead, rq.nd,
                              rq.trials, rq.max_enum, 0xC0FFEE42ull, g_ncpu, &r);
        double dt = now_ms() - t0;

        EnterCriticalSection(&g_cs);
        if (rq.gen == g_gen) { g_wres = r; g_wms = dt; g_wrc = rc; }
        LeaveCriticalSection(&g_cs);
        PostMessage(g_hwnd, WM_APP_RESULT, (WPARAM)rq.gen, (LPARAM)rc);
    }
}

/* current board from the board slots */
static int cur_board(int out[5])
{
    int n = 0;
    for (int i = 0; i < 5; i++)
        if (slot_card[SLOT_BOARD + i] >= 0) out[n++] = slot_card[SLOT_BOARD + i];
    return n;
}

static int cur_board2(int out[5])
{
    int n = 0;
    for (int i = 0; i < 5; i++)
        if (slot_card[SLOT_BOARD2 + i] >= 0) out[n++] = slot_card[SLOT_BOARD2 + i];
    return n;
}

/* board tables must not be queried while the worker may be rebuilding them */
static int worker_idle(void) { return g_done_gen == g_gen; }

static const wchar_t *street_name(int nb)
{
    return nb == 0 ? L"pre" : nb == 3 ? L"flop" : nb == 4 ? L"turn" : L"river";
}

/* percentile of a hand in the distribution for the current board(s):
 * uses the matching board table when built, else the preflop table.
 * *tag is set to the street label of whichever table answered. */
static double pct_for_display(const int cards[5], const wchar_t **tag)
{
    int b[5], nb = cur_board(b);
    if (g_db) {
        int b2[5], nb2 = cur_board2(b2);
        if (nb >= 3 && nb2 >= 3 && worker_idle()) {
            double v = plo5_hand_percentile_2b(cards, b, nb, b2, nb2);
            if (v >= 0) { *tag = L"2 bds"; return v; }
        }
    } else if (nb >= 3 && worker_idle()) {
        double v = plo5_hand_percentile_on(cards, b, nb);
        if (v >= 0) { *tag = street_name(nb); return v; }
    }
    *tag = street_name(0);
    return g_ranks ? plo5_hand_percentile(cards) : -1.0;
}

/* build a request from the model; 0 = ok, else hint set */
static int validate_state(req_t *rq)
{
    memset(rq, 0, sizeof *rq);
    rq->np = g_np;
    g_hint[0] = 0;

    rq->db = g_db;
    for (int i = 0; i < 5; i++)
        if (slot_card[SLOT_BOARD + i] >= 0)
            rq->board[rq->nb++] = slot_card[SLOT_BOARD + i];
    if (rq->nb == 1 || rq->nb == 2) {
        swprintf(g_hint, 192, L"Board%s needs 3, 4 or 5 cards (or none)",
                 g_db ? L" A" : L"");
        return -1;
    }
    if (g_db) {
        for (int i = 0; i < 5; i++)
            if (slot_card[SLOT_BOARD2 + i] >= 0)
                rq->board2[rq->nb2++] = slot_card[SLOT_BOARD2 + i];
        if (rq->nb2 == 1 || rq->nb2 == 2) {
            swprintf(g_hint, 192, L"Board B needs 3, 4 or 5 cards (or none)");
            return -1;
        }
    }

    for (int p = 0; p < g_np; p++) {
        rq->pl[p].type = g_mode[p];
        if (g_mode[p] == PLO5_P_FIXED) {
            int n = 0;
            for (int i = 0; i < 5; i++)
                if (slot_card[p * 5 + i] >= 0)
                    rq->pl[p].cards[n++] = slot_card[p * 5 + i];
            if (n < 5) {
                swprintf(g_hint, 192,
                         L"Player %d needs %d more card%s — click the deck or "
                         L"type e.g.  a h  for A♥", p + 1, 5 - n,
                         5 - n == 1 ? L"" : L"s");
                return -1;
            }
        } else if (g_mode[p] == PLO5_P_RANGE) {
            if (rq->nb == 0 && (!g_db || rq->nb2 == 0) && !g_ranks) {
                swprintf(g_hint, 192, L"Preflop ranges need the rank table — "
                         L"run plo5calc --gen-ranks");
                return -1;
            }
            if (g_db && (rq->nb > 0) != (rq->nb2 > 0)) {
                swprintf(g_hint, 192, L"Double-board ranges need both boards "
                         L"set (or both empty)");
                return -1;
            }
            if (g_hi[p] <= g_lo[p]) {
                swprintf(g_hint, 192,
                         L"Player %d: empty range (%.0f–%.0f)", p + 1,
                         g_lo[p], g_hi[p]);
                return -1;
            }
            rq->pl[p].lo = g_lo[p];
            rq->pl[p].hi = g_hi[p];
        }
    }
    for (int i = 0; i < NDEAD; i++)
        if (slot_card[SLOT_DEAD + i] >= 0)
            rq->dead[rq->nd++] = slot_card[SLOT_DEAD + i];

    rq->trials = PREC[g_prec].trials;
    rq->max_enum = PREC[g_prec].max_enum;
    return 0;
}

/* queue a build of the board ranking for the current board(s) */
static void rank_board_now(void)
{
    req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.db = g_db;
    rq.nb = cur_board(rq.board);
    if (g_db) rq.nb2 = cur_board2(rq.board2);
    if (rq.nb < 3 || (g_db && rq.nb2 < 3)) {
        swprintf(g_hint, 192, g_db
                 ? L"Set at least a flop on BOTH boards first"
                 : L"Set a flop, turn or river first — preflop "
                   L"already uses the static table");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    rq.build_only = 1;
    g_err[0] = 0;
    EnterCriticalSection(&g_cs);
    rq.gen = ++g_gen;
    g_req = rq;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_evt);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void calc_now(void)
{
    req_t rq;
    if (validate_state(&rq) != 0) {
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    g_err[0] = 0;
    EnterCriticalSection(&g_cs);
    rq.gen = ++g_gen;
    g_req = rq;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_evt);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void update_qhand(void);

static void state_changed(void)
{
    g_stale = 1;
    req_t tmp;
    validate_state(&tmp);           /* refresh hint text */
    update_qhand();                 /* street may have changed */
    if (g_auto) calc_now();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Slot / model logic                                                  */
/* ------------------------------------------------------------------ */

static int slot_usable(int s)
{
    if (s < SLOT_BOARD) {
        int p = s / 5;
        return p < g_np && g_mode[p] == PLO5_P_FIXED;
    }
    if (s >= SLOT_BOARD2) return g_db;                     /* board B */
    if (s >= SLOT_DEAD)                                     /* dead */
        return s - SLOT_DEAD < (g_db ? NDEAD_DB : NDEAD);
    return 1;                                               /* board A */
}

/* deal order: players, board A, then board B in double mode */
static int deal_order(int *out)
{
    int n = 0;
    for (int p = 0; p < g_np; p++)
        if (g_mode[p] == PLO5_P_FIXED)
            for (int i = 0; i < 5; i++) out[n++] = p * 5 + i;
    for (int i = 0; i < 5; i++) out[n++] = SLOT_BOARD + i;
    if (g_db)
        for (int i = 0; i < 5; i++) out[n++] = SLOT_BOARD2 + i;
    return n;
}

static int next_empty(int after)
{
    if (after >= SLOT_DEAD && after < SLOT_DEAD + NDEAD) {  /* stay in dead */
        int lim = SLOT_DEAD + (g_db ? NDEAD_DB : NDEAD);
        for (int s = after + 1; s < lim; s++)
            if (slot_card[s] < 0) return s;
        return -1;
    }
    int ord[NSLOTS], n = deal_order(ord), from = -1;
    for (int i = 0; i < n; i++)
        if (ord[i] == after) { from = i; break; }
    for (int i = from + 1; i < n; i++)
        if (slot_card[ord[i]] < 0) return ord[i];
    return -1;
}

static void clear_slot(int s)
{
    if (slot_card[s] >= 0) {
        card_slot[slot_card[s]] = -1;
        slot_card[s] = -1;
    }
}

static void place_card(int c)
{
    if (card_slot[c] >= 0) {
        clear_slot(card_slot[c]);
        state_changed();
        return;
    }
    if (g_active < 0 || !slot_usable(g_active)) return;
    clear_slot(g_active);
    slot_card[g_active] = c;
    card_slot[c] = g_active;
    if (g_active < SLOT_BOARD) g_sel_p = g_active / 5;
    g_active = next_empty(g_active);
    apply_layout();                 /* deal-button label may change */
    state_changed();
}

static void clear_player(int p)
{
    for (int i = 0; i < 5; i++) clear_slot(p * 5 + i);
    if (g_mode[p] == PLO5_P_FIXED) g_active = p * 5;
    g_sel_p = p;
    apply_layout();
    state_changed();
}

static void clear_all(void)
{
    for (int s = 0; s < NSLOTS; s++) clear_slot(s);
    g_active = 0;
    for (int p = 0; p < NP_MAX; p++) {
        if (g_mode[p] == PLO5_P_FIXED) continue;
    }
    apply_layout();
    state_changed();
}

static void sync_range_edits(int p)
{
    wchar_t b[16];
    g_syncing = 1;
    swprintf(b, 16, L"%.0f", g_lo[p]);
    SetWindowTextW(g_ed_lo[p], b);
    swprintf(b, 16, L"%.0f", g_hi[p]);
    SetWindowTextW(g_ed_hi[p], b);
    g_syncing = 0;
}

static void set_mode(int p, int mode)
{
    int bb[5], bb2[5];
    if (mode == PLO5_P_RANGE && !g_ranks &&
        (g_db ? (cur_board(bb) < 3 || cur_board2(bb2) < 3)
              : cur_board(bb) < 3)) return;
    if (g_mode[p] == mode) return;
    g_mode[p] = mode;
    if (mode != PLO5_P_FIXED) {
        for (int i = 0; i < 5; i++) clear_slot(p * 5 + i);
        if (g_active >= p * 5 && g_active < p * 5 + 5)
            g_active = next_empty(p * 5 + 4);
    } else {
        g_active = p * 5;
    }
    if (mode == PLO5_P_RANGE) sync_range_edits(p);
    g_sel_p = p;
    apply_layout();
    state_changed();
}

static const wchar_t *g_qtag = L"pre";

static void update_qhand(void)
{
    int b[5], nb = cur_board(b), got = 0;
    int b2[5], nb2 = g_db ? cur_board2(b2) : 0;
    if (g_db && nb >= 3 && nb2 >= 3 && worker_idle() &&
        plo5_percentile_hand_2b(g_qpct, b, nb, b2, nb2, g_qhand) == PLO5_OK) {
        g_qtag = L"2 bds";
        got = 1;
    } else if (!g_db && nb >= 3 && worker_idle() &&
        plo5_percentile_hand_on(g_qpct, b, nb, g_qhand) == PLO5_OK) {
        g_qtag = street_name(nb);
        got = 1;
    } else if (g_ranks && plo5_percentile_hand(g_qpct, g_qhand) == PLO5_OK) {
        g_qtag = street_name(0);
        got = 1;
    }
    if (got) {
        /* sort descending for display */
        for (int i = 1; i < 5; i++) {
            int v = g_qhand[i], j = i - 1;
            while (j >= 0 && g_qhand[j] < v) { g_qhand[j + 1] = g_qhand[j]; j--; }
            g_qhand[j + 1] = v;
        }
    } else {
        g_qhand[0] = -1;
    }
}

/* deal a random flop/turn/river (street = 3/4/5 cards) to the board —
 * to BOTH boards in double mode — from cards not used anywhere else */
static void random_board(int street)
{
    for (int i = 0; i < 5; i++) {
        clear_slot(SLOT_BOARD + i);
        if (g_db) clear_slot(SLOT_BOARD2 + i);
    }
    int pool[52], n = 0;
    for (int c = 0; c < 52; c++)
        if (card_slot[c] < 0) pool[n++] = c;
    int need = street * (g_db ? 2 : 1);
    if (n < need) return;               /* cannot happen in practice */
    for (int i = 0; i < need; i++) {
        int j = i + rnd_below(n - i);
        int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (int i = 0; i < street; i++) {
        slot_card[SLOT_BOARD + i] = pool[i];
        card_slot[pool[i]] = SLOT_BOARD + i;
        if (g_db) {
            slot_card[SLOT_BOARD2 + i] = pool[street + i];
            card_slot[pool[street + i]] = SLOT_BOARD2 + i;
        }
    }
    g_active = next_empty(-1);          /* first open player slot */
    apply_layout();
    state_changed();
}

/* launch the quiz trainer sitting next to this exe */
static void launch_quiz(void)
{
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = len ? wcsrchr(path, L'\\') : NULL;
    if (slash) wcscpy(slash + 1, L"plo5quiz.exe");
    else wcscpy(path, L"plo5quiz.exe");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        swprintf(g_hint, 192, L"plo5quiz.exe not found next to the calculator");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void deal_hand_to_sel(const int hand[5])
{
    if (hand[0] < 0 || g_sel_p >= g_np) return;
    int p = g_sel_p;
    g_mode[p] = PLO5_P_FIXED;
    for (int i = 0; i < 5; i++) clear_slot(p * 5 + i);
    for (int i = 0; i < 5; i++) {
        int c = hand[i];
        if (card_slot[c] >= 0) clear_slot(card_slot[c]);   /* steal */
        slot_card[p * 5 + i] = c;
        card_slot[c] = p * 5 + i;
    }
    g_active = next_empty(p * 5 + 4);
    apply_layout();
    state_changed();
}

static void deal_qhand(void) { deal_hand_to_sel(g_qhand); }

/* queue the equity-finder search */
static void find_eq_now(void)
{
    req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.db = g_db;
    rq.nb = cur_board(rq.board);
    if (g_db) rq.nb2 = cur_board2(rq.board2);
    if (rq.nb == 1 || rq.nb == 2 || (g_db && (rq.nb2 == 1 || rq.nb2 == 2))) {
        swprintf(g_hint, 192, L"Finish the board first (0, 3, 4 or 5 cards)");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (g_db && (rq.nb > 0) != (rq.nb2 > 0)) {
        swprintf(g_hint, 192, L"Double board: set both boards (or neither)");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (rq.nb == 0 && !g_ranks) {
        swprintf(g_hint, 192, L"Preflop lookups need the rank table — "
                 L"run plo5calc --gen-ranks");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    rq.find_eq = 1;
    rq.target = g_eq_target / 100.0;
    g_err[0] = 0;
    g_hint[0] = 0;
    EnterCriticalSection(&g_cs);
    rq.gen = ++g_gen;
    g_req = rq;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_evt);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void rrect(HDC dc, const RECT *r, COLORREF fill, COLORREF border, int bw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, bw, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, S(7), S(7));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void dtext(HDC dc, const RECT *r, const wchar_t *s, COLORREF c,
                  HFONT f, UINT flags)
{
    RECT rr = *r;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f);
    DrawTextW(dc, s, -1, &rr, flags | DT_NOPREFIX);
}

#define DT_MID (DT_CENTER | DT_VCENTER | DT_SINGLELINE)
#define DT_LV  (DT_LEFT | DT_VCENTER | DT_SINGLELINE)

static void draw_card_face(HDC dc, const RECT *r, int card, int dim,
                           int hot, int active)
{
    COLORREF bg = dim ? C_CARD_DIM : hot ? C_HOT : C_CARD;
    COLORREF bd = active ? C_SEL : C_BORDER;
    rrect(dc, r, bg, bd, active ? S(2) : 1);
    wchar_t t[3] = { RANKW[card >> 2], SUITW[card & 3], 0 };
    dtext(dc, r, t, dim ? C_TXT_DIM : suit_col(card & 3), g_f_card, DT_MID);
}

static void draw_empty_slot(HDC dc, const RECT *r, int active, int hot)
{
    rrect(dc, r, active ? C_SEL_BG : hot ? C_HOT : C_EMPTY,
          active ? C_SEL : C_BORDER, active ? S(2) : 1);
}

/* draw a 5-card hand as colored text, returns nothing; clips to rect */
static void draw_hand_text(HDC dc, int x, int y, const int *cards, HFONT f)
{
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f);
    for (int i = 0; i < 5; i++) {
        if (cards[i] < 0) return;
        wchar_t t[3] = { RANKW[cards[i] >> 2], SUITW[cards[i] & 3], 0 };
        SetTextColor(dc, suit_col(cards[i] & 3));
        TextOutW(dc, x, y, t, 2);
        SIZE sz;
        GetTextExtentPoint32W(dc, t, 2, &sz);
        x += sz.cx + S(2);
    }
}

static void fmt_u64(uint64_t v, wchar_t *out)
{
    wchar_t tmp[32];
    swprintf(tmp, 32, L"%llu", (unsigned long long)v);
    int len = lstrlenW(tmp), o = 0;
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = L',';
        out[o++] = tmp[i];
    }
    out[o] = 0;
}

static int track_x_of_pct(const RECT *tr, double pct)
{
    return tr->left + (int)((tr->right - tr->left) * pct / 100.0 + 0.5);
}

static void draw_all(HDC dc)
{
    RECT cli = { 0, 0, g_cw, g_ch };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &cli, bg);
    DeleteObject(bg);

    /* deck grid */
    for (int c = 0; c < 52; c++)
        draw_card_face(dc, &g_card_r[c], c, card_slot[c] >= 0,
                       g_hot_card == c, 0);

    /* board + dead */
    RECT lr = { S(8), g_slot_r[SLOT_BOARD].top, S(56), g_slot_r[SLOT_BOARD].bottom };
    dtext(dc, &lr, g_db ? L"Bd A" : L"Board", C_TXT_SOFT, g_f_lbl,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (g_db) {
        RECT lb = g_slot_r[SLOT_BOARD2];
        lb.right = lb.left - S(6); lb.left -= S(46);
        dtext(dc, &lb, L"Bd B", C_TXT_SOFT, g_f_lbl,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT lr2 = g_slot_r[SLOT_DEAD];
    lr2.right = lr2.left - S(6); lr2.left -= S(48);
    dtext(dc, &lr2, L"Dead", C_TXT_SOFT, g_f_lbl, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    dtext(dc, &g_rndlbl_r, L"random board", C_TXT_DIM, g_f_sm, DT_MID);
    for (int s = SLOT_BOARD; s < NSLOTS; s++) {
        if (s >= SLOT_BOARD2 && !g_db) continue;
        if (s >= SLOT_DEAD && s < SLOT_DEAD + NDEAD &&
            s - SLOT_DEAD >= (g_db ? NDEAD_DB : NDEAD)) continue;
        if (slot_card[s] >= 0)
            draw_card_face(dc, &g_slot_r[s], slot_card[s], 0,
                           g_hot_slot == s, g_active == s);
        else
            draw_empty_slot(dc, &g_slot_r[s], g_active == s, g_hot_slot == s);
    }

    /* player rows */
    for (int p = 0; p < g_np; p++) {
        RECT pr = g_prow_r[p];
        wchar_t lbl[8], t[128];
        swprintf(lbl, 8, L"P%d", p + 1);
        RECT plr = { pr.left, pr.top, pr.left + S(30), pr.bottom };
        dtext(dc, &plr, lbl, g_sel_p == p ? PCOL[p] : C_TXT_SOFT, g_f_lbl, DT_LV);

        if (g_mode[p] == PLO5_P_FIXED) {
            int complete = 1;
            for (int i = 0; i < 5; i++) {
                int s = p * 5 + i;
                if (slot_card[s] >= 0)
                    draw_card_face(dc, &g_slot_r[s], slot_card[s], 0,
                                   g_hot_slot == s, g_active == s);
                else {
                    draw_empty_slot(dc, &g_slot_r[s], g_active == s,
                                    g_hot_slot == s);
                    complete = 0;
                }
            }
            if (complete) {
                int cd[5];
                for (int i = 0; i < 5; i++) cd[i] = slot_card[p * 5 + i];
                const wchar_t *tag;
                double pc = pct_for_display(cd, &tag);
                if (pc >= 0) {
                    swprintf(t, 128, L"%.1f\n%s", pc, tag);
                    RECT tr = { S(368), pr.top + S(8), S(414), pr.bottom - S(8) };
                    dtext(dc, &tr, t, C_TXT_SOFT, g_f_sm, DT_CENTER | DT_WORDBREAK);
                }
            }
        } else if (g_mode[p] == PLO5_P_RANGE) {
            /* numeric edits are child windows; draw the dash + % */
            RECT dr = { S(220), pr.top + S(6), S(238), pr.top + S(30) };
            dtext(dc, &dr, L"–", C_TXT_SOFT, g_f_ui, DT_MID);
            RECT pctr = { S(286), pr.top + S(6), S(330), pr.top + S(30) };
            dtext(dc, &pctr, L"pctile", C_TXT_SOFT, g_f_sm, DT_LV);

            /* slider */
            RECT tr = g_track_r[p];
            rrect(dc, &tr, C_BAR_BG, C_BAR_BG, 1);
            RECT band = tr;
            band.left = track_x_of_pct(&tr, g_lo[p]);
            band.right = track_x_of_pct(&tr, g_hi[p]);
            if (band.right > band.left)
                rrect(dc, &band, PCOL[p], PCOL[p], 1);
            for (int hnd = 0; hnd < 2; hnd++) {
                int cx = track_x_of_pct(&tr, hnd ? g_hi[p] : g_lo[p]);
                int cy = (tr.top + tr.bottom) / 2, rr2 = S(8);
                HBRUSH hb = CreateSolidBrush(C_CARD);
                HPEN hp = CreatePen(PS_SOLID, S(2), PCOL[p]);
                HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, hp);
                Ellipse(dc, cx - rr2, cy - rr2, cx + rr2, cy + rr2);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(hb); DeleteObject(hp);
            }
        } else {
            RECT tr = { S(174), pr.top, S(414), pr.bottom };
            dtext(dc, &tr, L"random hand", C_TXT_DIM, g_f_ui, DT_LV);
        }

        /* results */
        if (g_res_ok && p < g_res_np) {
            int bx = S(452), bw = S(240);
            COLORREF fillc = g_stale ? mixc(PCOL[p], C_BG, 55) : PCOL[p];
            COLORREF txtc = g_stale ? C_TXT_DIM : C_TXT;
            RECT bar = { bx, pr.top + S(15), bx + bw, pr.top + S(35) };
            rrect(dc, &bar, C_BAR_BG, C_BAR_BG, 1);
            int fw = (int)(bw * g_res.equity[p] + 0.5);
            if (fw > S(4)) {
                RECT fill = { bx, bar.top, bx + fw, bar.bottom };
                rrect(dc, &fill, fillc, fillc, 1);
            }
            swprintf(t, 128, L"%.2f%%", g_res.equity[p] * 100.0);
            RECT tr = { S(700), pr.top + S(6), S(800), pr.top + S(38) };
            dtext(dc, &tr, t, txtc, g_f_big, DT_LV);
            if (g_res.exact)
                swprintf(t, 128, L"win %.2f%%   tie %.2f%%",
                         g_res.win[p] * 100.0, g_res.tie[p] * 100.0);
            else
                swprintf(t, 128, L"win %.2f%%   tie %.2f%%   ±%.2f%%",
                         g_res.win[p] * 100.0, g_res.tie[p] * 100.0,
                         g_res.ci95[p] * 100.0);
            RECT sr = { bx, bar.bottom + S(3), g_cw - S(14), pr.bottom };
            dtext(dc, &sr, t, g_stale ? C_TXT_DIM : C_TXT_SOFT, g_f_sm,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }

    /* panel labels */
    int px = S(532);
    RECT t1 = { px, S(55), px + S(64), S(81) };
    dtext(dc, &t1, L"Players", C_TXT, g_f_ui, DT_LV);
    wchar_t nb[8];
    swprintf(nb, 8, L"%d", g_np);
    RECT tc = { px + S(96), S(52), px + S(134), S(78) };
    dtext(dc, &tc, nb, C_TXT, g_f_ui, DT_MID);
    RECT t2 = { px, S(89), px + S(64), S(115) };
    dtext(dc, &t2, L"Precision", C_TXT, g_f_ui, DT_LV);
    RECT t3 = { px, S(164), px + S(64), S(188) };
    dtext(dc, &t3, L"Pct hand", C_TXT, g_f_ui, DT_LV);
    RECT t4 = { px, S(198), px + S(64), S(222) };
    dtext(dc, &t4, L"Eq hand", C_TXT, g_f_ui, DT_LV);

    /* equity-finder result: hand + measured equity; click hand to deal */
    if (g_ehand[0] >= 0) {
        draw_hand_text(dc, g_ehand_r.left, g_ehand_r.top + S(3), g_ehand,
                       g_f_ui);
        wchar_t et[24];
        swprintf(et, 24, L"= %.1f%%", g_ehand_eq * 100.0);
        RECT etr = { g_ehand_r.right + S(4), g_ehand_r.top,
                     g_cw - S(14), g_ehand_r.bottom };
        dtext(dc, &etr, et, C_TXT_SOFT, g_f_sm, DT_LV);
    } else {
        RECT etr = { g_ehand_r.left, g_ehand_r.top,
                     g_cw - S(14), g_ehand_r.bottom };
        dtext(dc, &etr, L"vs 1 random, this street", C_TXT_DIM, g_f_sm, DT_LV);
    }

    /* percentile query hand + street tag */
    if (g_qhand[0] >= 0) {
        draw_hand_text(dc, g_qhand_r.left, g_qhand_r.top + S(3), g_qhand, g_f_ui);
        RECT tagr = { g_qhand_r.right - S(42), g_qhand_r.top,
                      g_qhand_r.right, g_qhand_r.bottom };
        dtext(dc, &tagr, g_qtag, C_TXT_DIM, g_f_sm, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (!g_ranks) {
        dtext(dc, &g_qhand_r, L"needs rank table", C_TXT_DIM, g_f_sm, DT_LV);
    }

    /* buttons */
    for (int i = 0; i < g_nbtn; i++) {
        btn_t *b = &g_btn[i];
        COLORREF bgc = b->dis ? C_EMPTY : b->on ? C_SEL :
                       (g_hot_btn == i ? C_HOT : C_CARD);
        COLORREF fg = b->dis ? C_TXT_DIM : b->on ? RGB(255, 255, 255) : C_TXT;
        HFONT f = b->id == B_CALC ? g_f_calc : g_f_ui;
        if (b->id == B_CALC && !b->dis && g_stale) {
            bgc = C_SEL;
            fg = RGB(255, 255, 255);
        }
        rrect(dc, &b->r, bgc, b->on || (b->id == B_CALC && g_stale) ? C_SEL : C_BORDER, 1);
        dtext(dc, &b->r, b->label, fg, f, DT_MID);
    }

    /* status (panel) */
    wchar_t st[384];
    st[0] = 0;
    if (g_res_ok) {
        wchar_t n[32];
        fmt_u64(g_res.samples, n);
        if (g_res.exact)
            swprintf(st, 384, L"Exact — %s runouts · %.0f ms\n", n, g_res_ms);
        else
            swprintf(st, 384, L"Monte Carlo — %s trials · %.0f ms · %d thr\n",
                     n, g_res_ms, g_ncpu);
    }
    wcscat(st, g_ranks ? L"Preflop ranks: loaded · "
                       : L"Preflop ranks: missing (plo5calc --gen-ranks) · ");
    if (g_db) {
        int b1[5], b2[5], n1 = 0, n2 = 0;
        if (worker_idle() && plo5_board_ranks_state2(b1, &n1, b2, &n2))
            wcscat(st, L"2-board ranks: built\n");
        else
            wcscat(st, L"2-board ranks: none\n");
        wcscat(st, L"Each board pays half; best hand on both boards\n"
                   L"scoops. Ranges rank hands across both boards.");
    } else {
        int bb[5], bn = 0;
        if (worker_idle() && plo5_board_ranks_state(bb, &bn)) {
            wchar_t bt[64], cs[4];
            char c8[3];
            swprintf(bt, 64, L"board ranks: %s ", street_name(bn));
            for (int i = 0; i < bn; i++) {
                plo5_card_str(bb[i], c8);
                cs[0] = c8[0]; cs[1] = c8[1]; cs[2] = 0;
                wcscat(bt, cs);
            }
            wcscat(st, bt);
            wcscat(st, L"\n");
        } else {
            wcscat(st, L"board ranks: none\n");
        }
        wcscat(st, L"Ranges: 0 = weakest … 100 = strongest,\n"
                   L"on the current board (preflop if none)");
    }
    dtext(dc, &g_status_r, st, C_TXT_SOFT, g_f_sm, DT_LEFT | DT_TOP | DT_WORDBREAK);

    /* bottom bar */
    const wchar_t *bl = NULL;
    COLORREF bc = C_TXT_SOFT;
    wchar_t tmp[224];
    if (g_done_gen != g_gen) {
        bl = L"computing…";
        bc = C_SEL;
    } else if (g_err[0]) {
        bl = g_err;
        bc = RGB(200, 40, 40);
    } else if (g_hint[0]) {
        bl = g_hint;
        bc = C_WARN;
    } else if (g_stale) {
        bl = L"Results out of date — press Calculate (Enter), or turn on Auto";
        bc = C_WARN;
    } else {
        swprintf(tmp, 224, L"Click deck to assign · right-click removes · "
                 L"type  a h  → A♥ · Enter = Calculate");
        bl = tmp;
    }
    dtext(dc, &g_bottom_r, bl, bc, g_f_sm, DT_LV);
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */

static void copy_results(void)
{
    if (!g_res_ok) return;
    wchar_t buf[1200], line[160];
    buf[0] = 0;
    char cs[3];
    for (int p = 0; p < g_res_np; p++) {
        wchar_t hand[32] = L"random";
        if (g_mode[p] == PLO5_P_FIXED) {
            int o = 0;
            for (int i = 0; i < 5; i++) {
                if (slot_card[p * 5 + i] < 0) break;
                plo5_card_str(slot_card[p * 5 + i], cs);
                hand[o++] = cs[0]; hand[o++] = cs[1];
            }
            hand[o] = 0;
        } else if (g_mode[p] == PLO5_P_RANGE) {
            swprintf(hand, 32, L"%.0f-%.0f pct", g_lo[p], g_hi[p]);
        }
        swprintf(line, 160, L"P%d %-14s equity %6.2f%%  win %6.2f%%  tie %5.2f%%",
                 p + 1, hand, g_res.equity[p] * 100.0, g_res.win[p] * 100.0,
                 g_res.tie[p] * 100.0);
        lstrcatW(buf, line);
        lstrcatW(buf, L"\r\n");
    }
    if (g_db) lstrcatW(buf, L"(double board, split pot)\r\n");
    int nbo = 0;
    wchar_t bs[32] = L"";
    for (int i = 0; i < 5; i++)
        if (slot_card[SLOT_BOARD + i] >= 0) {
            plo5_card_str(slot_card[SLOT_BOARD + i], cs);
            bs[nbo * 2] = cs[0]; bs[nbo * 2 + 1] = cs[1]; bs[nbo * 2 + 2] = 0;
            nbo++;
        }
    if (nbo) {
        swprintf(line, 160, g_db ? L"board A %s\r\n" : L"board %s\r\n", bs);
        lstrcatW(buf, line);
    }
    if (g_db) {
        nbo = 0;
        bs[0] = 0;
        for (int i = 0; i < 5; i++)
            if (slot_card[SLOT_BOARD2 + i] >= 0) {
                plo5_card_str(slot_card[SLOT_BOARD2 + i], cs);
                bs[nbo * 2] = cs[0]; bs[nbo * 2 + 1] = cs[1]; bs[nbo * 2 + 2] = 0;
                nbo++;
            }
        if (nbo) {
            swprintf(line, 160, L"board B %s\r\n", bs);
            lstrcatW(buf, line);
        }
    }
    wchar_t n[32];
    fmt_u64(g_res.samples, n);
    swprintf(line, 160, g_res.exact ? L"(exact, %s runouts)\r\n"
                                    : L"(Monte Carlo, %s trials)\r\n", n);
    lstrcatW(buf, line);

    size_t bytes = ((size_t)lstrlenW(buf) + 1) * sizeof(wchar_t);
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), buf, bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

/* ------------------------------------------------------------------ */
/* Hit testing / input                                                 */
/* ------------------------------------------------------------------ */

static int hit_rect(const RECT *r, POINT p)
{
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

static int hit_card(POINT p)
{
    for (int c = 0; c < 52; c++) if (hit_rect(&g_card_r[c], p)) return c;
    return -1;
}

static int hit_slot(POINT p)
{
    for (int pp = 0; pp < g_np; pp++)
        if (g_mode[pp] == PLO5_P_FIXED)
            for (int i = 0; i < 5; i++)
                if (hit_rect(&g_slot_r[pp * 5 + i], p)) return pp * 5 + i;
    for (int s = SLOT_BOARD; s < NSLOTS; s++)
        if (slot_usable(s) && hit_rect(&g_slot_r[s], p)) return s;
    return -1;
}

static int hit_btn(POINT p)
{
    for (int i = 0; i < g_nbtn; i++) if (hit_rect(&g_btn[i].r, p)) return i;
    return -1;
}

/* returns player if p is over a range slider track (with slack) */
static int hit_track(POINT pt)
{
    for (int p = 0; p < g_np; p++) {
        if (g_mode[p] != PLO5_P_RANGE) continue;
        RECT r = g_track_r[p];
        InflateRect(&r, S(10), S(10));
        if (hit_rect(&r, pt)) return p;
    }
    return -1;
}

static void on_button(int id)
{
    if (id == B_PMINUS && g_np > 2) {
        g_np--;
        for (int i = 0; i < 5; i++) clear_slot(g_np * 5 + i);
        g_mode[g_np] = PLO5_P_FIXED;
        if (g_sel_p >= g_np) g_sel_p = 0;
        if (g_active >= g_np * 5 && g_active < SLOT_BOARD) g_active = 0;
        resize_window();
        state_changed();
        return;
    }
    if (id == B_PPLUS && g_np < NP_MAX) {
        g_np++;
        sync_range_edits(g_np - 1);
        resize_window();
        state_changed();
        return;
    }
    if (id == B_PREC) {
        g_prec = (g_prec + 1) % 3;
        apply_layout();
        state_changed();
        return;
    }
    if (id == B_CLEARALL) { clear_all(); return; }
    if (id == B_COPY) { copy_results(); return; }
    if (id == B_CALC) { calc_now(); return; }
    if (id == B_AUTO) {
        g_auto = !g_auto;
        apply_layout();
        if (g_auto && g_stale) calc_now();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (id == B_DEAL) { deal_qhand(); return; }
    if (id == B_RANKB) { rank_board_now(); return; }
    if (id == B_QUIZ) { launch_quiz(); return; }
    if (id == B_FINDEQ) { find_eq_now(); return; }
    if (id == B_RFLOP) { random_board(3); return; }
    if (id == B_RTURN) { random_board(4); return; }
    if (id == B_RRIVER) { random_board(5); return; }
    if (id == B_SB || id == B_DBL) {
        int want = id == B_DBL;
        if (want == g_db) return;
        g_db = want;
        if (g_db) {
            /* only NDEAD_DB dead slots visible in double mode */
            for (int i = NDEAD_DB; i < NDEAD; i++) clear_slot(SLOT_DEAD + i);
        } else {
            for (int i = 0; i < 5; i++) clear_slot(SLOT_BOARD2 + i);
        }
        if (g_active >= SLOT_BOARD2 && !g_db) g_active = SLOT_BOARD;
        apply_layout();
        state_changed();
        return;
    }
    if (id >= B_MODE0 && id < B_MODE0 + NP_MAX * 4) {
        int p = (id - B_MODE0) / 4, m = (id - B_MODE0) % 4;
        set_mode(p, m == 0 ? PLO5_P_FIXED : m == 1 ? PLO5_P_RANGE : PLO5_P_RANDOM);
        return;
    }
    if (id >= B_PCLR0 && id < B_PCLR0 + NP_MAX) {
        clear_player(id - B_PCLR0);
        return;
    }
}

static void on_char(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z') ch = ch - L'A' + L'a';
    int suit = -1, rank = -1;
    if (ch == L'c') suit = 0;
    else if (ch == L'd') suit = 1;
    else if (ch == L'h') suit = 2;
    else if (ch == L's') suit = 3;
    if (ch >= L'2' && ch <= L'9') rank = ch - L'2';
    else if (ch == L't') rank = 8;
    else if (ch == L'j') rank = 9;
    else if (ch == L'q') rank = 10;
    else if (ch == L'k') rank = 11;
    else if (ch == L'a') rank = 12;

    if (g_pend_rank >= 0 && suit >= 0) {
        int card = g_pend_rank * 4 + suit;
        g_pend_rank = -1;
        if (card_slot[card] < 0) place_card(card);
        return;
    }
    if (rank >= 0) g_pend_rank = rank;
}

static void drag_track(int p, int x)
{
    RECT tr = g_track_r[p];
    double pct = (double)(x - tr.left) * 100.0 / (double)(tr.right - tr.left);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pct = (double)(int)(pct + 0.5);
    if (g_drag_hi) {
        g_hi[p] = pct < g_lo[p] ? g_lo[p] : pct;
    } else {
        g_lo[p] = pct > g_hi[p] ? g_hi[p] : pct;
    }
    sync_range_edits(p);
    state_changed();
}

/* ------------------------------------------------------------------ */
/* Window proc                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        draw_all(mem);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        SetFocus(h);
        int b = hit_btn(p);
        if (b >= 0) {
            if (!g_btn[b].dis) on_button(g_btn[b].id);
            return 0;
        }
        if (g_ehand[0] >= 0 && hit_rect(&g_ehand_r, p)) {
            deal_hand_to_sel(g_ehand);      /* click the found hand = deal */
            return 0;
        }
        int tp = hit_track(p);
        if (tp >= 0) {
            RECT tr = g_track_r[tp];
            int xlo = track_x_of_pct(&tr, g_lo[tp]);
            int xhi = track_x_of_pct(&tr, g_hi[tp]);
            g_drag_p = tp;
            g_drag_hi = abs(p.x - xhi) < abs(p.x - xlo) ||
                        (abs(p.x - xhi) == abs(p.x - xlo) && p.x > xhi);
            g_sel_p = tp;
            SetCapture(h);
            drag_track(tp, p.x);
            return 0;
        }
        int s = hit_slot(p);
        if (s >= 0) {
            if (slot_usable(s)) {
                g_active = s;
                if (s < SLOT_BOARD) g_sel_p = s / 5;
                apply_layout();
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        int c = hit_card(p);
        if (c >= 0) place_card(c);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (g_drag_p >= 0) {
            drag_track(g_drag_p, p.x);
            return 0;
        }
        int hc = hit_card(p), hs = hit_slot(p), hb = hit_btn(p);
        if (hc != g_hot_card || hs != g_hot_slot || hb != g_hot_btn) {
            g_hot_card = hc; g_hot_slot = hs; g_hot_btn = hb;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag_p >= 0) {
            g_drag_p = -1;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int s = hit_slot(p);
        if (s >= 0 && slot_card[s] >= 0) { clear_slot(s); state_changed(); return 0; }
        int c = hit_card(p);
        if (c >= 0 && card_slot[c] >= 0) { clear_slot(card_slot[c]); state_changed(); }
        return 0;
    }
    case WM_CHAR:
        on_char((wchar_t)w);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_RETURN) { calc_now(); return 0; }
        if (w == VK_ESCAPE) { g_pend_rank = -1; return 0; }
        if (w == VK_DELETE || w == VK_BACK) {
            if (g_active >= 0 && slot_card[g_active] >= 0) {
                clear_slot(g_active);
                state_changed();
            }
            return 0;
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (HIWORD(w) == EN_CHANGE && !g_syncing) {
            wchar_t txt[16];
            GetWindowTextW((HWND)l, txt, 16);
            double v = _wtof(txt);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            if (id >= 200 && id < 200 + NP_MAX) {
                g_lo[id - 200] = v;
                state_changed();
            } else if (id >= 220 && id < 220 + NP_MAX) {
                g_hi[id - 220] = v;
                state_changed();
            } else if (id == 240) {
                g_qpct = v;
                update_qhand();
                InvalidateRect(h, NULL, FALSE);
            } else if (id == 241) {
                g_eq_target = v;
            }
        }
        return 0;
    }
    case WM_APP_RESULT:
        EnterCriticalSection(&g_cs);
        if ((unsigned)w == g_gen) {
            int build_only = g_req.build_only;
            if (g_req.find_eq) {
                if (g_wrc == PLO5_OK && g_wehand[0] >= 0) {
                    memcpy(g_ehand, g_wehand, sizeof g_ehand);
                    /* sort descending for display */
                    for (int i = 1; i < 5; i++) {
                        int v = g_ehand[i], j = i - 1;
                        while (j >= 0 && g_ehand[j] < v) {
                            g_ehand[j + 1] = g_ehand[j];
                            j--;
                        }
                        g_ehand[j + 1] = v;
                    }
                    g_ehand_eq = g_weq;
                    g_err[0] = 0;
                } else {
                    swprintf(g_err, 128, L"equity search failed (%d)", g_wrc);
                }
                g_done_gen = (unsigned)w;
                LeaveCriticalSection(&g_cs);
                update_qhand();
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            if (g_wrc == PLO5_OK) {
                if (!build_only) {
                    g_res = g_wres;
                    g_res_ms = g_wms;
                    g_res_np = g_req.np;
                    g_res_ok = 1;
                    g_stale = 0;
                }
                g_err[0] = 0;
            } else {
                swprintf(g_err, 128, L"engine error %d%s", g_wrc,
                         g_wrc == PLO5_ERR_RANGE ?
                         L" — range clashes with dead cards" : L"");
            }
            g_done_gen = (unsigned)w;
        }
        LeaveCriticalSection(&g_cs);
        update_qhand();             /* board table may have changed */
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        make_fonts();
        apply_layout();
        for (int p = 0; p < NP_MAX; p++) {
            SendMessage(g_ed_lo[p], WM_SETFONT, (WPARAM)g_f_ui, TRUE);
            SendMessage(g_ed_hi[p], WM_SETFONT, (WPARAM)g_f_ui, TRUE);
        }
        SendMessage(g_ed_pct, WM_SETFONT, (WPARAM)g_f_ui, TRUE);
        RECT *r = (RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI *SetCtx)(HANDLE);
    SetCtx set_ctx = (SetCtx)(void *)GetProcAddress(u32,
                                       "SetProcessDpiAwarenessContext");
    if (set_ctx) set_ctx((HANDLE)-4);

    plo5_init();

    /* rank table lives next to the exe */
    {
        char path[512];
        DWORD len = GetModuleFileNameA(NULL, path, sizeof path);
        char *slash = (len > 0) ? strrchr(path, '\\') : NULL;
        if (slash) strcpy(slash + 1, "plo5rank.bin");
        else strcpy(path, "plo5rank.bin");
        g_ranks = plo5_ranks_load(path) == PLO5_OK;
    }

    for (int s = 0; s < NSLOTS; s++) slot_card[s] = -1;
    for (int c = 0; c < 52; c++) card_slot[c] = -1;
    for (int p = 0; p < NP_MAX; p++) { g_lo[p] = 0; g_hi[p] = 100; }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_ncpu = (int)si.dwNumberOfProcessors;
    if (g_ncpu < 1) g_ncpu = 1;

    LARGE_INTEGER qc;
    QueryPerformanceCounter(&qc);
    g_rng = (uint64_t)qc.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);

    InitializeCriticalSection(&g_cs);
    g_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, worker, NULL, 0, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"plo5gui";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"plo5gui", L"PLO5 Equity Calculator",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                           NULL, NULL, hi, NULL);
    UINT dpi = GetDpiForWindow(g_hwnd);
    if (dpi) g_dpi = (int)dpi;
    make_fonts();

    for (int p = 0; p < NP_MAX; p++) {
        g_ed_lo[p] = CreateWindowW(L"EDIT", L"0",
            WS_CHILD | WS_BORDER | ES_CENTER | ES_NUMBER,
            0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)(200 + p), hi, NULL);
        g_ed_hi[p] = CreateWindowW(L"EDIT", L"100",
            WS_CHILD | WS_BORDER | ES_CENTER | ES_NUMBER,
            0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)(220 + p), hi, NULL);
        SendMessage(g_ed_lo[p], WM_SETFONT, (WPARAM)g_f_ui, TRUE);
        SendMessage(g_ed_hi[p], WM_SETFONT, (WPARAM)g_f_ui, TRUE);
    }
    g_ed_pct = CreateWindowW(L"EDIT", L"30",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_NUMBER,
        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)240, hi, NULL);
    SendMessage(g_ed_pct, WM_SETFONT, (WPARAM)g_f_ui, TRUE);
    g_ed_eq = CreateWindowW(L"EDIT", L"55",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER,
        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)241, hi, NULL);
    SendMessage(g_ed_eq, WM_SETFONT, (WPARAM)g_f_ui, TRUE);

    update_qhand();
    resize_window();
    {
        req_t tmp;
        validate_state(&tmp);       /* set the initial hint */
    }
    ShowWindow(g_hwnd, show);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
