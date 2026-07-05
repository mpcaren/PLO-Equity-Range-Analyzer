/* quiz.c — PLO5 equity estimation trainer.
 *
 * Deals a random hand, a random flop and a random number of opponents
 * (full random ranges), asks for your equity estimate, grades it, and
 * tracks your average error. True equity and the hand's flop percentile
 * are computed in the background while you think.
 *
 * Build (MSVC):  cl /O2 /std:c11 /utf-8 quiz.c plo5.c /Fe:plo5quiz.exe /link /subsystem:windows
 * Build (gcc) :  gcc -O3 -std=c11 -mwindows -o plo5quiz quiz.c plo5.c -lgdi32
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plo5.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

/* ------------------------------------------------------------------ */
/* Look (matches plo5gui)                                              */
/* ------------------------------------------------------------------ */

#define C_BG       RGB(243, 244, 246)
#define C_CARD     RGB(255, 255, 255)
#define C_BORDER   RGB(203, 206, 212)
#define C_HOT      RGB(226, 237, 252)
#define C_SEL      RGB(26, 115, 232)
#define C_TXT      RGB(32, 33, 36)
#define C_TXT_SOFT RGB(95, 99, 104)
#define C_TXT_DIM  RGB(178, 181, 188)
#define C_BAR_BG   RGB(224, 226, 230)
#define C_GOOD     RGB(24, 128, 56)
#define C_MID      RGB(200, 108, 0)
#define C_BAD      RGB(200, 40, 40)

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

static HFONT g_f_ui, g_f_sm, g_f_card, g_f_big, g_f_lbl;
static int   g_dpi = 96;
static HWND  g_hwnd, g_edit;
static int   g_ncpu = 1;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void make_fonts(void)
{
    if (g_f_ui) {
        DeleteObject(g_f_ui); DeleteObject(g_f_sm); DeleteObject(g_f_card);
        DeleteObject(g_f_big); DeleteObject(g_f_lbl);
    }
    #define MKF(px, w) CreateFontW(-S(px), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, \
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, \
        DEFAULT_PITCH, L"Segoe UI")
    g_f_ui   = MKF(15, FW_NORMAL);
    g_f_sm   = MKF(12, FW_NORMAL);
    g_f_card = MKF(22, FW_SEMIBOLD);
    g_f_big  = MKF(26, FW_SEMIBOLD);
    g_f_lbl  = MKF(13, FW_SEMIBOLD);
    #undef MKF
}

/* ------------------------------------------------------------------ */
/* Quiz state                                                          */
/* ------------------------------------------------------------------ */

static int    g_hero[5], g_flop[3], g_flop2[3], g_nopp = 2;
static int    g_nopp_sel = 0;           /* 0 = random, 1..5 = fixed */
static int    g_db = 0;                 /* double board split pot */
static int    g_qnum = 0;
static int    g_phase = 0;              /* 0 = thinking, 1 = reveal */
static double g_guess = -1;
static int    g_answered = 0;
static double g_err_sum = 0, g_err_last = 0, g_err_best = 1e9, g_err_worst = 0;

static plo5_result g_qres;
static int      g_res_ready = 0, g_pct_ready = 0;
static double   g_hero_pct = -1;
static unsigned g_gen = 0;
static wchar_t  g_hintw[96];

typedef struct { int hero[5], flop[3], flop2[3], nopp, db; unsigned gen; } qreq_t;
static qreq_t          g_req;
static CRITICAL_SECTION g_cs;
static HANDLE          g_evt;
static plo5_result     g_wres;
static double          g_wpct;

#define WM_APP_EQ  (WM_APP + 1)
#define WM_APP_PCT (WM_APP + 2)

/* simple splitmix for dealing */
static uint64_t g_rng;
static uint64_t rnd64(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static int rnd_below(int n) { return (int)(((rnd64() >> 32) * (uint64_t)n) >> 32); }

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_evt, INFINITE);
        EnterCriticalSection(&g_cs);
        qreq_t rq = g_req;
        LeaveCriticalSection(&g_cs);

        plo5_player pl[PLO5_MAX_PLAYERS];
        memset(pl, 0, sizeof pl);
        pl[0].type = PLO5_P_FIXED;
        memcpy(pl[0].cards, rq.hero, sizeof rq.hero);
        for (int p = 1; p <= rq.nopp; p++) pl[p].type = PLO5_P_RANDOM;

        plo5_result r;
        int rc = rq.db
            ? plo5_equity_2b(pl, rq.nopp + 1, rq.flop, 3, rq.flop2, 3,
                             NULL, 0, 500000, 0, rnd64(), g_ncpu, &r)
            : plo5_equity2(pl, rq.nopp + 1, rq.flop, 3, NULL, 0,
                           500000, 0, rnd64(), g_ncpu, &r);
        EnterCriticalSection(&g_cs);
        if (rq.gen == g_gen && rc == PLO5_OK) g_wres = r;
        LeaveCriticalSection(&g_cs);
        if (rc == PLO5_OK)
            PostMessage(g_hwnd, WM_APP_EQ, (WPARAM)rq.gen, 0);

        /* percentile of the hero hand on this flop / both flops (~1-4 s) */
        int prc = rq.db
            ? plo5_board_ranks_build2(rq.flop, 3, rq.flop2, 3, 100, g_ncpu,
                                      NULL, NULL)
            : plo5_board_ranks_build(rq.flop, 3, 100, g_ncpu, NULL, NULL);
        if (prc == PLO5_OK) {
            double pc = rq.db
                ? plo5_hand_percentile_2b(rq.hero, rq.flop, 3, rq.flop2, 3)
                : plo5_hand_percentile_on(rq.hero, rq.flop, 3);
            EnterCriticalSection(&g_cs);
            if (rq.gen == g_gen) g_wpct = pc;
            LeaveCriticalSection(&g_cs);
            PostMessage(g_hwnd, WM_APP_PCT, (WPARAM)rq.gen, 0);
        }
    }
}

static void new_question(void)
{
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 0; i < 11; i++) {
        int j = i + rnd_below(52 - i);
        int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    memcpy(g_hero, deck, 5 * sizeof(int));
    memcpy(g_flop, deck + 5, 3 * sizeof(int));
    memcpy(g_flop2, deck + 8, 3 * sizeof(int));
    /* sort hero desc for display */
    for (int i = 1; i < 5; i++) {
        int v = g_hero[i], j = i - 1;
        while (j >= 0 && g_hero[j] < v) { g_hero[j + 1] = g_hero[j]; j--; }
        g_hero[j + 1] = v;
    }
    g_nopp = g_nopp_sel ? g_nopp_sel : 1 + rnd_below(5);
    g_qnum++;
    g_phase = 0;
    g_guess = -1;
    g_res_ready = g_pct_ready = 0;
    g_hero_pct = -1;
    g_hintw[0] = 0;
    SetWindowTextW(g_edit, L"");
    SetFocus(g_edit);

    qreq_t rq;
    memcpy(rq.hero, g_hero, sizeof g_hero);
    memcpy(rq.flop, g_flop, sizeof g_flop);
    memcpy(rq.flop2, g_flop2, sizeof g_flop2);
    rq.nopp = g_nopp;
    rq.db = g_db;
    EnterCriticalSection(&g_cs);
    rq.gen = ++g_gen;
    g_req = rq;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_evt);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void submit_guess(void)
{
    wchar_t txt[32];
    GetWindowTextW(g_edit, txt, 32);
    double v = _wtof(txt);
    if (txt[0] == 0 || v < 0 || v > 100) {
        lstrcpynW(g_hintw, L"Enter your equity estimate as 0–100", 96);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    g_guess = v;
    g_phase = 1;
    g_hintw[0] = 0;
    if (g_res_ready) {
        double err = v - g_qres.equity[0] * 100.0;
        if (err < 0) err = -err;
        g_answered++;
        g_err_sum += err;
        g_err_last = err;
        if (err < g_err_best) g_err_best = err;
        if (err > g_err_worst) g_err_worst = err;
    }
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_enter(void)
{
    if (g_phase == 0) submit_guess();
    else new_question();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void rrect(HDC dc, const RECT *r, COLORREF fill, COLORREF border, int bw)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, bw, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, S(8), S(8));
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

static void draw_cards(HDC dc, const int *cards, int n, int cx, int y,
                       int cw, int ch)
{
    int gap = S(8);
    int total = n * cw + (n - 1) * gap;
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        RECT r = { x, y, x + cw, y + ch };
        rrect(dc, &r, C_CARD, C_BORDER, 1);
        wchar_t t[3] = { RANKW[cards[i] >> 2], SUITW[cards[i] & 3], 0 };
        dtext(dc, &r, t, suit_col(cards[i] & 3), g_f_card, DT_MID);
        x += cw + gap;
    }
}

static const wchar_t *grade_of(double err, COLORREF *col)
{
    if (err <= 1.5) { *col = C_GOOD; return L"perfect"; }
    if (err <= 3)   { *col = C_GOOD; return L"excellent"; }
    if (err <= 6)   { *col = C_MID;  return L"good"; }
    if (err <= 10)  { *col = C_MID;  return L"rough"; }
    *col = C_BAD;   return L"way off";
}

static RECT g_btn_r, g_dbbtn_r, g_opp_r[6];
static int  g_btn_hot = 0;

/* window size and control positions depend on the double-board toggle */
static void quiz_layout(void)
{
    int yin = g_db ? 382 : 296;
    if (g_edit) MoveWindow(g_edit, S(336), S(yin), S(70), S(28), TRUE);
    g_btn_r.left = S(416); g_btn_r.top = S(yin - 2);
    g_btn_r.right = g_btn_r.left + S(104);
    g_btn_r.bottom = g_btn_r.top + S(32);

    /* settings row */
    g_dbbtn_r.left = S(20); g_dbbtn_r.top = S(44);
    g_dbbtn_r.right = S(200); g_dbbtn_r.bottom = S(70);
    for (int i = 0; i < 6; i++) {
        g_opp_r[i].left = S(304) + i * S(32) + (i ? S(12) : 0);
        g_opp_r[i].right = g_opp_r[i].left + (i ? S(28) : S(40));
        g_opp_r[i].top = S(44);
        g_opp_r[i].bottom = S(70);
    }

    if (g_hwnd) {
        RECT r = { 0, 0, S(600), S(g_db ? 652 : 562) };
        AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                         WS_MINIMIZEBOX, FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void draw_all(HDC dc, int W, int H)
{
    RECT cli = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &cli, bg);
    DeleteObject(bg);

    wchar_t t[192];
    int cx = W / 2;

    /* header */
    swprintf(t, 192, L"Question %d", g_qnum);
    RECT hr = { S(20), S(14), W / 2, S(38) };
    dtext(dc, &hr, t, C_TXT, g_f_lbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (g_answered > 0)
        swprintf(t, 192, L"answered %d · avg error %.1fpp · best %.1f · worst %.1f",
                 g_answered, g_err_sum / g_answered, g_err_best, g_err_worst);
    else
        lstrcpynW(t, L"estimate your equity, press Enter", 192);
    RECT sr = { W / 2 - S(60), S(14), W - S(20), S(38) };
    dtext(dc, &sr, t, C_TXT_SOFT, g_f_sm, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    /* settings row: double-board toggle + opponent count */
    rrect(dc, &g_dbbtn_r, g_db ? C_SEL : C_CARD, g_db ? C_SEL : C_BORDER, 1);
    dtext(dc, &g_dbbtn_r, g_db ? L"Double board: ON" : L"Double board: OFF",
          g_db ? RGB(255, 255, 255) : C_TXT, g_f_sm, DT_MID);
    RECT ol = { S(212), S(44), S(300), S(70) };
    dtext(dc, &ol, L"Opponents", C_TXT_SOFT, g_f_sm,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < 6; i++) {
        int on = g_nopp_sel == i;
        rrect(dc, &g_opp_r[i], on ? C_SEL : C_CARD, on ? C_SEL : C_BORDER, 1);
        wchar_t ob[4];
        if (i == 0) lstrcpynW(ob, L"Rnd", 4);
        else swprintf(ob, 4, L"%d", i);
        dtext(dc, &g_opp_r[i], ob, on ? RGB(255, 255, 255) : C_TXT,
              g_f_sm, DT_MID);
    }

    /* hero hand */
    RECT l1 = { 0, S(78), W, S(98) };
    dtext(dc, &l1, L"YOUR HAND", C_TXT_SOFT, g_f_lbl, DT_MID);
    draw_cards(dc, g_hero, 5, cx, S(102), S(52), S(70));

    /* flop(s) + opponents */
    if (g_db)
        swprintf(t, 192, L"FLOP A — vs %d random opponent%s · each board "
                 L"pays half", g_nopp, g_nopp == 1 ? L"" : L"s");
    else
        swprintf(t, 192, L"FLOP — vs %d random opponent%s (full range)",
                 g_nopp, g_nopp == 1 ? L"" : L"s");
    RECT l2 = { 0, S(184), W, S(204) };
    dtext(dc, &l2, t, C_TXT_SOFT, g_f_lbl, DT_MID);
    draw_cards(dc, g_flop, 3, cx, S(208), S(52), S(70));
    if (g_db) {
        RECT l2b = { 0, S(282), W, S(302) };
        dtext(dc, &l2b, L"FLOP B", C_TXT_SOFT, g_f_lbl, DT_MID);
        draw_cards(dc, g_flop2, 3, cx, S(306), S(52), S(70));
    }

    /* input row */
    int yin = g_db ? 382 : 296;
    RECT l3 = { S(20), S(yin + 2), cx + S(28), S(yin + 28) };
    dtext(dc, &l3, L"Your equity estimate (%):", C_TXT, g_f_ui,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    /* the edit control sits at cx+36 */

    /* submit / next button */
    COLORREF bb = g_btn_hot ? C_HOT : C_CARD;
    int active = g_phase == 0 || g_res_ready;
    rrect(dc, &g_btn_r, g_phase == 0 ? C_SEL : bb,
          g_phase == 0 ? C_SEL : C_BORDER, 1);
    dtext(dc, &g_btn_r, g_phase == 0 ? L"Submit" : L"Next hand",
          g_phase == 0 ? RGB(255, 255, 255) : (active ? C_TXT : C_TXT_DIM),
          g_f_ui, DT_MID);

    if (g_hintw[0]) {
        RECT hri = { 0, S(yin + 36), W, S(yin + 56) };
        dtext(dc, &hri, g_hintw, C_MID, g_f_sm, DT_MID);
    }

    /* reveal */
    int yrv = g_db ? 462 : 368;
    if (g_phase == 1) {
        if (!g_res_ready) {
            RECT wr = { 0, S(yrv + 4), W, S(yrv + 34) };
            dtext(dc, &wr, L"computing…", C_SEL, g_f_ui, DT_MID);
        } else {
            double eq = g_qres.equity[0] * 100.0;
            double err = g_guess - eq;
            double aerr = err < 0 ? -err : err;
            COLORREF gc;
            const wchar_t *grade = grade_of(aerr, &gc);

            /* bar 0..100 with truth fill and guess marker */
            int bx = S(60), bw = W - S(120), by = S(yrv), bh = S(24);
            RECT bar = { bx, by, bx + bw, by + bh };
            rrect(dc, &bar, C_BAR_BG, C_BAR_BG, 1);
            RECT fill = { bx, by, bx + (int)(bw * eq / 100.0 + 0.5), by + bh };
            if (fill.right > fill.left + S(3))
                rrect(dc, &fill, C_SEL, C_SEL, 1);
            int gx = bx + (int)(bw * g_guess / 100.0 + 0.5);
            HPEN mp = CreatePen(PS_SOLID, S(3), gc);
            HGDIOBJ op = SelectObject(dc, mp);
            MoveToEx(dc, gx, by - S(6), NULL);
            LineTo(dc, gx, by + bh + S(6));
            SelectObject(dc, op);
            DeleteObject(mp);
            RECT gl = { gx - S(40), by + bh + S(6), gx + S(40), by + bh + S(24) };
            dtext(dc, &gl, L"your guess", gc, g_f_sm, DT_MID);

            swprintf(t, 192, L"Equity  %.1f%%", eq);
            RECT er = { 0, S(yrv + 54), W, S(yrv + 90) };
            dtext(dc, &er, t, C_TXT, g_f_big, DT_MID);

            swprintf(t, 192, L"you said %.1f — error %.1fpp (%s)",
                     g_guess, aerr, grade);
            RECT gr = { 0, S(yrv + 92), W, S(yrv + 116) };
            dtext(dc, &gr, t, gc, g_f_ui, DT_MID);

            const wchar_t *wl = g_db ? L"scoop" : L"win";
            if (g_pct_ready && g_hero_pct >= 0)
                swprintf(t, 192,
                         L"%s %.1f%% · tie %.1f%% · your hand is the %.0fth "
                         L"percentile holding %s",
                         wl, g_qres.win[0] * 100.0, g_qres.tie[0] * 100.0,
                         g_hero_pct,
                         g_db ? L"across both flops" : L"on this flop");
            else
                swprintf(t, 192, L"%s %.1f%% · tie %.1f%%",
                         wl, g_qres.win[0] * 100.0, g_qres.tie[0] * 100.0);
            RECT wr2 = { 0, S(yrv + 118), W, S(yrv + 140) };
            dtext(dc, &wr2, t, C_TXT_SOFT, g_f_sm, DT_MID);
        }
    }

    RECT fr = { 0, H - S(28), W, H - S(8) };
    dtext(dc, &fr, L"Enter = submit / next question", C_TXT_DIM, g_f_sm, DT_MID);
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
        draw_all(mem, cr.right, cr.bottom);
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
        if (p.x >= g_dbbtn_r.left && p.x < g_dbbtn_r.right &&
            p.y >= g_dbbtn_r.top && p.y < g_dbbtn_r.bottom) {
            g_db = !g_db;
            quiz_layout();
            new_question();
            return 0;
        }
        for (int i = 0; i < 6; i++)
            if (p.x >= g_opp_r[i].left && p.x < g_opp_r[i].right &&
                p.y >= g_opp_r[i].top && p.y < g_opp_r[i].bottom) {
                if (g_nopp_sel != i) {
                    g_nopp_sel = i;
                    new_question();
                }
                return 0;
            }
        if (p.x >= g_btn_r.left && p.x < g_btn_r.right &&
            p.y >= g_btn_r.top && p.y < g_btn_r.bottom)
            do_enter();
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hot = p.x >= g_btn_r.left && p.x < g_btn_r.right &&
                  p.y >= g_btn_r.top && p.y < g_btn_r.bottom;
        if (hot != g_btn_hot) {
            g_btn_hot = hot;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_APP_EQ:
        EnterCriticalSection(&g_cs);
        if ((unsigned)w == g_gen) {
            g_qres = g_wres;
            g_res_ready = 1;
            /* guess was submitted before the result arrived */
            if (g_phase == 1 && g_guess >= 0) {
                double err = g_guess - g_qres.equity[0] * 100.0;
                if (err < 0) err = -err;
                g_answered++;
                g_err_sum += err;
                g_err_last = err;
                if (err < g_err_best) g_err_best = err;
                if (err > g_err_worst) g_err_worst = err;
            }
        }
        LeaveCriticalSection(&g_cs);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_APP_PCT:
        EnterCriticalSection(&g_cs);
        if ((unsigned)w == g_gen) {
            g_hero_pct = g_wpct;
            g_pct_ready = 1;
        }
        LeaveCriticalSection(&g_cs);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        make_fonts();
        SendMessage(g_edit, WM_SETFONT, (WPARAM)g_f_ui, TRUE);
        quiz_layout();
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
    wc.lpszClassName = L"plo5quiz";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"plo5quiz", L"PLO5 Equity Quiz",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                           NULL, NULL, hi, NULL);
    UINT dpi = GetDpiForWindow(g_hwnd);
    if (dpi) g_dpi = (int)dpi;
    make_fonts();

    g_edit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER,
        S(336), S(264), S(70), S(28),
        g_hwnd, (HMENU)(INT_PTR)100, hi, NULL);
    SendMessage(g_edit, WM_SETFONT, (WPARAM)g_f_ui, TRUE);

    quiz_layout();
    new_question();
    ShowWindow(g_hwnd, show);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            do_enter();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
