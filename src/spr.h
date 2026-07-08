/* spr.h — stack-off threshold analysis across SPRs for the PLO5 engine.
 *
 * Model: pot P on the current street, effective stacks S = SPR * P
 * behind, and `nway` players (hero + nopp villains) getting the rest in.
 * Villain continuing ranges are approximated by trimming a starting
 * percentile band down to its top MDF = 1/(1+SPR) fraction, ranked by
 * the board-conditional strength distribution the engine already builds.
 *
 * The range-construction step is deliberately separated from the
 * evaluation step: plo5_spr_table() is just plo5_mdf_trim_band() +
 * plo5_spr_row_eval() per SPR. To swap in a smarter continuing-range
 * model later, compute your own band (or per-combo keep set via
 * plo5_mdf_trim_scores) and call plo5_spr_row_eval() directly.
 *
 * Multiway (nway > 2) is rudimentary by design: every villain gets the
 * same flat MDF-trimmed band, and the thresholds assume all nway players
 * stack off for equal stacks S. With nway == 2 the formulas reduce to
 * the classic heads-up ones: eq_needed = SPR/(1+2*SPR), MDF = 1/(1+SPR).
 */
#ifndef PLO5_SPR_H
#define PLO5_SPR_H

#include <stdint.h>
#include "plo5.h"

#ifdef __cplusplus
extern "C" {
#endif

/* one output row of the SPR grid */
typedef struct {
    double spr;                /* input SPR                              */
    double stack;              /* S = SPR * P                            */
    double pot_final;          /* P + nway * S (everyone stacks off)     */
    double mdf;                /* 1 / (1 + SPR)                          */
    double trim_lo, trim_hi;   /* villain continuing band actually used  */
    double eq_needed;          /* raw stack-off threshold, no fold equity */
    double hero_eq;            /* hero equity vs the trimmed range(s)    */
    double ci95;               /* MC 95% CI half-width on hero_eq        */
    int    profitable_no_fold; /* 1 if hero_eq >= eq_needed              */
    double breakeven_fold;     /* fold% needed when not profitable, else 0:
                                  (eq_needed - hero_eq) / (1 - hero_eq),
                                  from EV = F*P + (1-F)*(eq*pot_final - S) */
    int    band_widened;       /* 1 if trim_lo had to be relaxed below the
                                  MDF cut because that many disjoint hands
                                  from the tight band cannot be dealt
                                  (multiway, card-clustered tops of range) */
} plo5_spr_row;

/* ---- pure stack-off math (no engine calls) ------------------------- */

double plo5_spr_stack(double spr, double pot);               /* SPR * P  */
double plo5_spr_pot_final(double spr, double pot, int nway); /* P+nway*S */
double plo5_spr_mdf(double spr);                             /* 1/(1+SPR) */

/* equity needed to break even stacking off with no fold equity:
 * S / (P + nway*S) = SPR / (1 + nway*SPR). nway == 2 is heads-up. */
double plo5_spr_eq_needed(double spr, int nway);

/* fold frequency needed when hero_eq < eq_needed (clamped to [0,1]);
 * returns 0 when hero_eq already clears the threshold */
double plo5_breakeven_fold(double eq_needed, double hero_eq);

/* trim a starting percentile band [lo,hi] to its top `mdf` fraction */
void plo5_mdf_trim_band(double lo, double hi, double mdf,
                        double *trim_lo, double *trim_hi);

/* generic combo-list trimming: given n strength scores (any scale,
 * higher = stronger), mark the top ceil(mdf*n) in keep[] (1/0) and
 * return how many were kept. Ties broken toward keeping earlier
 * entries. This is the hook for smarter range construction later. */
int plo5_mdf_trim_scores(const double *scores, int n, double mdf,
                         unsigned char *keep);

/* ---- engine-backed evaluation --------------------------------------- */

/* Evaluate one SPR row: hero (5 cards) vs nopp villains, each drawing
 * from the percentile band [trim_lo, trim_hi] conditional on `board`
 * (0 or 3/4/5 cards; 0 uses the preflop rank table). The band is used
 * as given — trim it yourself (this is the swap-in point).
 * Requires the matching rank table: plo5_ranks_load()ed for preflop,
 * or plo5_board_ranks_build() run for the board (plo5_equity2 builds
 * it automatically, but building once up front is much faster across
 * a whole grid). Returns PLO5_OK or a PLO5_ERR_* code. */
int plo5_spr_row_eval(const int hero[5], const int *board, int nboard,
                      double trim_lo, double trim_hi, int nopp,
                      double pot, double spr,
                      uint64_t trials, uint64_t seed, int nthreads,
                      plo5_spr_row *row);

/* Full grid: for each SPR, MDF-trim the villain starting band
 * [villain_lo, villain_hi] and evaluate. Builds the board ranking once
 * up front. rows must hold nspr entries. nopp in 1..PLO5_MAX_PLAYERS-1.
 * Multiway, a tight band can be undealable (nopp disjoint hands all in
 * the top few percent may not exist): the band floor is then relaxed in
 * small steps toward villain_lo until dealing succeeds, and the row is
 * flagged band_widened with trim_lo showing the band actually used. */
int plo5_spr_table(const int hero[5], const int *board, int nboard,
                   double villain_lo, double villain_hi, int nopp,
                   double pot, const double *sprs, int nspr,
                   uint64_t trials, uint64_t seed, int nthreads,
                   plo5_spr_row *rows);

#ifdef __cplusplus
}
#endif
#endif /* PLO5_SPR_H */
