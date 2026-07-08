/* plo5_r.c — R bindings for the PLO5 equity engine (.Call interface). */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include "plo5.h"
#include "spr.h"

static void ensure_init(void)
{
    static int done = 0;
    if (!done) { plo5_init(); done = 1; }
}

/* types: int vec (0 fixed / 1 random / 2 range)
 * cards: int vec length np*5 (card ids, anything for non-fixed)
 * lows/highs: double vec length np (percentiles for range players) */
SEXP C_plo5_equity(SEXP types, SEXP cards, SEXP lows, SEXP highs,
                   SEXP board, SEXP board2, SEXP dbl, SEXP dead,
                   SEXP trials, SEXP maxenum, SEXP seed, SEXP threads)
{
    ensure_init();
    int np = LENGTH(types);
    if (np < 2 || np > PLO5_MAX_PLAYERS)
        error("need 2..%d players", PLO5_MAX_PLAYERS);
    plo5_player pl[PLO5_MAX_PLAYERS];
    memset(pl, 0, sizeof pl);
    int *ty = INTEGER(types), *cd = INTEGER(cards);
    double *lo = REAL(lows), *hi = REAL(highs);
    for (int p = 0; p < np; p++) {
        pl[p].type = ty[p];
        for (int i = 0; i < 5; i++) pl[p].cards[i] = cd[p * 5 + i];
        pl[p].lo = lo[p];
        pl[p].hi = hi[p];
    }
    plo5_result r;
    int rc;
    if (asLogical(dbl))
        rc = plo5_equity_2b(pl, np, LENGTH(board) ? INTEGER(board) : NULL,
                            LENGTH(board),
                            LENGTH(board2) ? INTEGER(board2) : NULL,
                            LENGTH(board2),
                            LENGTH(dead) ? INTEGER(dead) : NULL,
                            LENGTH(dead), (uint64_t)asReal(trials),
                            (uint64_t)asReal(maxenum), (uint64_t)asReal(seed),
                            asInteger(threads), &r);
    else
        rc = plo5_equity2(pl, np, LENGTH(board) ? INTEGER(board) : NULL,
                          LENGTH(board), LENGTH(dead) ? INTEGER(dead) : NULL,
                          LENGTH(dead), (uint64_t)asReal(trials),
                          (uint64_t)asReal(maxenum), (uint64_t)asReal(seed),
                          asInteger(threads), &r);
    if (rc != PLO5_OK) error("plo5 equity failed (code %d)", rc);

    SEXP out = PROTECT(allocVector(VECSXP, 6));
    SEXP nm = PROTECT(allocVector(STRSXP, 6));
    const char *nms[6] = { "equity", "win", "tie", "ci95", "samples", "exact" };
    SEXP eq = PROTECT(allocVector(REALSXP, np));
    SEXP wn = PROTECT(allocVector(REALSXP, np));
    SEXP ti = PROTECT(allocVector(REALSXP, np));
    SEXP ci = PROTECT(allocVector(REALSXP, np));
    for (int p = 0; p < np; p++) {
        REAL(eq)[p] = r.equity[p];
        REAL(wn)[p] = r.win[p];
        REAL(ti)[p] = r.tie[p];
        REAL(ci)[p] = r.ci95[p];
    }
    SET_VECTOR_ELT(out, 0, eq);
    SET_VECTOR_ELT(out, 1, wn);
    SET_VECTOR_ELT(out, 2, ti);
    SET_VECTOR_ELT(out, 3, ci);
    SET_VECTOR_ELT(out, 4, ScalarReal((double)r.samples));
    SET_VECTOR_ELT(out, 5, ScalarLogical(r.exact));
    for (int i = 0; i < 6; i++) SET_STRING_ELT(nm, i, mkChar(nms[i]));
    setAttrib(out, R_NamesSymbol, nm);
    UNPROTECT(6);
    return out;
}

SEXP C_plo5_ranks_load(SEXP path)
{
    ensure_init();
    int rc = plo5_ranks_load(CHAR(STRING_ELT(path, 0)));
    return ScalarLogical(rc == PLO5_OK);
}

SEXP C_plo5_ranks_generate(SEXP path, SEXP trials, SEXP threads, SEXP seed)
{
    ensure_init();
    int rc = plo5_ranks_generate(CHAR(STRING_ELT(path, 0)),
                                 (uint32_t)asReal(trials),
                                 asInteger(threads), (uint64_t)asReal(seed),
                                 NULL, NULL);
    if (rc != PLO5_OK) error("plo5_ranks_generate failed (code %d)", rc);
    return ScalarLogical(1);
}

SEXP C_plo5_board_ranks_build(SEXP board, SEXP board2, SEXP runouts,
                              SEXP threads)
{
    ensure_init();
    int rc = LENGTH(board2)
        ? plo5_board_ranks_build2(INTEGER(board), LENGTH(board),
                                  INTEGER(board2), LENGTH(board2),
                                  asInteger(runouts), asInteger(threads),
                                  NULL, NULL)
        : plo5_board_ranks_build(INTEGER(board), LENGTH(board),
                                 asInteger(runouts), asInteger(threads),
                                 NULL, NULL);
    if (rc != PLO5_OK) error("board ranking build failed (code %d)", rc);
    return ScalarLogical(1);
}

SEXP C_plo5_hand_percentile(SEXP cards, SEXP board, SEXP board2)
{
    ensure_init();
    double p;
    if (LENGTH(board2))
        p = plo5_hand_percentile_2b(INTEGER(cards),
                                    INTEGER(board), LENGTH(board),
                                    INTEGER(board2), LENGTH(board2));
    else
        p = plo5_hand_percentile_on(INTEGER(cards),
                                    LENGTH(board) ? INTEGER(board) : NULL,
                                    LENGTH(board));
    return ScalarReal(p);
}

SEXP C_plo5_percentile_hand(SEXP pct, SEXP board, SEXP board2)
{
    ensure_init();
    int h[5], rc;
    if (LENGTH(board2))
        rc = plo5_percentile_hand_2b(asReal(pct),
                                     INTEGER(board), LENGTH(board),
                                     INTEGER(board2), LENGTH(board2), h);
    else
        rc = plo5_percentile_hand_on(asReal(pct),
                                     LENGTH(board) ? INTEGER(board) : NULL,
                                     LENGTH(board), h);
    if (rc != PLO5_OK) error("percentile lookup failed (code %d) — load or "
                             "build the relevant rank table first", rc);
    SEXP out = PROTECT(allocVector(INTSXP, 5));
    for (int i = 0; i < 5; i++) INTEGER(out)[i] = h[i];
    UNPROTECT(1);
    return out;
}

/* SPR stack-off grid: hero (5 card ids) vs nopp MDF-trimmed range
 * villains. Returns a named list of column vectors (one entry per SPR). */
SEXP C_plo5_spr_table(SEXP hero, SEXP board, SEXP vlo, SEXP vhi, SEXP nopp,
                      SEXP pot, SEXP sprs, SEXP trials, SEXP seed,
                      SEXP threads)
{
    ensure_init();
    if (LENGTH(hero) != 5) error("hero needs exactly 5 cards");
    int nspr = LENGTH(sprs);
    if (nspr < 1) error("need at least one SPR");

    plo5_spr_row *rows = (plo5_spr_row *)R_alloc((size_t)nspr, sizeof *rows);
    int rc = plo5_spr_table(INTEGER(hero),
                            LENGTH(board) ? INTEGER(board) : NULL,
                            LENGTH(board),
                            asReal(vlo), asReal(vhi), asInteger(nopp),
                            asReal(pot), REAL(sprs), nspr,
                            (uint64_t)asReal(trials), (uint64_t)asReal(seed),
                            asInteger(threads), rows);
    if (rc != PLO5_OK) error("plo5_spr_table failed (code %d)", rc);

    const char *nms[12] = { "spr", "stack", "pot_final", "mdf",
                            "trim_lo", "trim_hi", "eq_needed", "hero_eq",
                            "ci95", "profitable_no_fold", "breakeven_fold",
                            "band_widened" };
    SEXP out = PROTECT(allocVector(VECSXP, 12));
    SEXP nm = PROTECT(allocVector(STRSXP, 12));
    for (int c = 0; c < 12; c++) {
        SEXP v = (c == 9 || c == 11) ? allocVector(LGLSXP, nspr)
                                     : allocVector(REALSXP, nspr);
        SET_VECTOR_ELT(out, c, v);
        SET_STRING_ELT(nm, c, mkChar(nms[c]));
    }
    for (int i = 0; i < nspr; i++) {
        REAL(VECTOR_ELT(out, 0))[i] = rows[i].spr;
        REAL(VECTOR_ELT(out, 1))[i] = rows[i].stack;
        REAL(VECTOR_ELT(out, 2))[i] = rows[i].pot_final;
        REAL(VECTOR_ELT(out, 3))[i] = rows[i].mdf;
        REAL(VECTOR_ELT(out, 4))[i] = rows[i].trim_lo;
        REAL(VECTOR_ELT(out, 5))[i] = rows[i].trim_hi;
        REAL(VECTOR_ELT(out, 6))[i] = rows[i].eq_needed;
        REAL(VECTOR_ELT(out, 7))[i] = rows[i].hero_eq;
        REAL(VECTOR_ELT(out, 8))[i] = rows[i].ci95;
        LOGICAL(VECTOR_ELT(out, 9))[i] = rows[i].profitable_no_fold;
        REAL(VECTOR_ELT(out, 10))[i] =
            rows[i].profitable_no_fold ? NA_REAL : rows[i].breakeven_fold;
        LOGICAL(VECTOR_ELT(out, 11))[i] = rows[i].band_widened;
    }
    setAttrib(out, R_NamesSymbol, nm);
    UNPROTECT(2);
    return out;
}

static const R_CallMethodDef calls[] = {
    { "C_plo5_equity",            (DL_FUNC)&C_plo5_equity,            12 },
    { "C_plo5_ranks_load",        (DL_FUNC)&C_plo5_ranks_load,         1 },
    { "C_plo5_ranks_generate",    (DL_FUNC)&C_plo5_ranks_generate,     4 },
    { "C_plo5_board_ranks_build", (DL_FUNC)&C_plo5_board_ranks_build,  4 },
    { "C_plo5_hand_percentile",   (DL_FUNC)&C_plo5_hand_percentile,    3 },
    { "C_plo5_percentile_hand",   (DL_FUNC)&C_plo5_percentile_hand,    3 },
    { "C_plo5_spr_table",         (DL_FUNC)&C_plo5_spr_table,         10 },
    { NULL, NULL, 0 }
};

void R_init_rplo5(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, calls, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
}
