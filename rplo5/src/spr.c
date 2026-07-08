/* spr.c — stack-off threshold analysis across SPRs. See spr.h. */
#include "spr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Pure math                                                            */
/* ------------------------------------------------------------------ */

double plo5_spr_stack(double spr, double pot)
{
    return spr * pot;
}

double plo5_spr_pot_final(double spr, double pot, int nway)
{
    return pot + (double)nway * plo5_spr_stack(spr, pot);
}

double plo5_spr_mdf(double spr)
{
    return 1.0 / (1.0 + spr);
}

double plo5_spr_eq_needed(double spr, int nway)
{
    return spr / (1.0 + (double)nway * spr);
}

double plo5_breakeven_fold(double eq_needed, double hero_eq)
{
    if (hero_eq >= eq_needed) return 0.0;
    if (hero_eq >= 1.0) return 0.0;              /* degenerate */
    double f = (eq_needed - hero_eq) / (1.0 - hero_eq);
    return f > 1.0 ? 1.0 : f < 0.0 ? 0.0 : f;
}

void plo5_mdf_trim_band(double lo, double hi, double mdf,
                        double *trim_lo, double *trim_hi)
{
    if (lo < 0) lo = 0;
    if (hi > 100) hi = 100;
    if (hi < lo) hi = lo;
    if (mdf < 0) mdf = 0;
    if (mdf > 1) mdf = 1;
    *trim_lo = hi - mdf * (hi - lo);
    *trim_hi = hi;
}

/* index sort helper for trim_scores */
typedef struct { double s; int i; } sc_t;

static int sc_cmp(const void *a, const void *b)
{
    const sc_t *x = a, *y = b;
    if (x->s != y->s) return x->s > y->s ? -1 : 1;  /* strongest first */
    return x->i < y->i ? -1 : 1;                     /* stable-ish ties */
}

int plo5_mdf_trim_scores(const double *scores, int n, double mdf,
                         unsigned char *keep)
{
    if (!scores || !keep || n <= 0) return 0;
    if (mdf < 0) mdf = 0;
    if (mdf > 1) mdf = 1;
    int nkeep = (int)ceil(mdf * (double)n);
    if (nkeep > n) nkeep = n;
    memset(keep, 0, (size_t)n);
    if (nkeep == 0) return 0;

    sc_t *v = malloc((size_t)n * sizeof *v);
    if (!v) return 0;
    for (int i = 0; i < n; i++) { v[i].s = scores[i]; v[i].i = i; }
    qsort(v, (size_t)n, sizeof *v, sc_cmp);
    for (int k = 0; k < nkeep; k++) keep[v[k].i] = 1;
    free(v);
    return nkeep;
}

/* ------------------------------------------------------------------ */
/* Engine-backed evaluation                                             */
/* ------------------------------------------------------------------ */

int plo5_spr_row_eval(const int hero[5], const int *board, int nboard,
                      double trim_lo, double trim_hi, int nopp,
                      double pot, double spr,
                      uint64_t trials, uint64_t seed, int nthreads,
                      plo5_spr_row *row)
{
    if (!hero || !row || pot <= 0 || spr < 0) return PLO5_ERR_ARG;
    if (nopp < 1 || nopp > PLO5_MAX_PLAYERS - 1) return PLO5_ERR_PLAYERS;

    int nway = nopp + 1;
    memset(row, 0, sizeof *row);
    row->spr       = spr;
    row->stack     = plo5_spr_stack(spr, pot);
    row->pot_final = plo5_spr_pot_final(spr, pot, nway);
    row->mdf       = plo5_spr_mdf(spr);
    row->trim_lo   = trim_lo;
    row->trim_hi   = trim_hi;
    row->eq_needed = plo5_spr_eq_needed(spr, nway);

    plo5_player pl[PLO5_MAX_PLAYERS];
    memset(pl, 0, sizeof pl);
    pl[0].type = PLO5_P_FIXED;
    memcpy(pl[0].cards, hero, 5 * sizeof(int));
    for (int p = 1; p <= nopp; p++) {
        pl[p].type = PLO5_P_RANGE;
        pl[p].lo = trim_lo;
        pl[p].hi = trim_hi;
    }

    plo5_result r;
    int rc = plo5_equity2(pl, nway, board, nboard, NULL, 0,
                          trials, 0, seed, nthreads, &r);
    if (rc != PLO5_OK) return rc;

    row->hero_eq            = r.equity[0];
    row->ci95               = r.ci95[0];
    row->profitable_no_fold = r.equity[0] >= row->eq_needed;
    row->breakeven_fold     = plo5_breakeven_fold(row->eq_needed, r.equity[0]);
    return PLO5_OK;
}

int plo5_spr_table(const int hero[5], const int *board, int nboard,
                   double villain_lo, double villain_hi, int nopp,
                   double pot, const double *sprs, int nspr,
                   uint64_t trials, uint64_t seed, int nthreads,
                   plo5_spr_row *rows)
{
    if (!sprs || nspr <= 0 || !rows) return PLO5_ERR_ARG;

    /* build the strength distribution once for the whole grid */
    if (nboard >= 3) {
        int rc = plo5_board_ranks_build(board, nboard, 100, nthreads,
                                        NULL, NULL);
        if (rc != PLO5_OK) return rc;
    } else if (!plo5_ranks_loaded()) {
        return PLO5_ERR_RANKS;
    }

    for (int i = 0; i < nspr; i++) {
        double tlo, thi;
        plo5_mdf_trim_band(villain_lo, villain_hi,
                           plo5_spr_mdf(sprs[i]), &tlo, &thi);
        /* multiway, nopp disjoint hands may not exist inside a tight
         * band (tops of range cluster on the same few cards): relax the
         * floor in 5-point steps until the deal succeeds */
        int widened = 0, rc;
        for (;;) {
            rc = plo5_spr_row_eval(hero, board, nboard, tlo, thi, nopp,
                                   pot, sprs[i], trials,
                                   seed + (uint64_t)i * 0x9E3779B9u,
                                   nthreads, &rows[i]);
            if (rc != PLO5_ERR_RANGE || tlo <= villain_lo + 1e-9) break;
            tlo = tlo - 5.0 > villain_lo ? tlo - 5.0 : villain_lo;
            widened = 1;
        }
        if (rc != PLO5_OK) return rc;
        rows[i].band_widened = widened;
    }
    return PLO5_OK;
}
