/* plo5.h — 5-card PLO (PLO5) equity library. C11, zero dependencies.
 *
 * Card encoding: id = rank*4 + suit, where rank 0='2' .. 12='A'
 * and suit 0='c', 1='d', 2='h', 3='s'.  Valid ids are 0..51.
 *
 * Designed to be wrapped from R via .Call: plo5_equity() takes flat int
 * arrays and writes results into a plain struct, no allocation of caller
 * data, no global state besides the (read-only after init) lookup tables.
 */
#ifndef PLO5_H
#define PLO5_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLO5_MAX_PLAYERS 6
#define PLO5_HAND_CARDS  5

/* A player whose 5 hand entries are all PLO5_RANDOM draws a random hand
 * each trial (Monte Carlo only; exact enumeration falls back to MC). */
#define PLO5_RANDOM (-1)

enum {
    PLO5_OK          =  0,
    PLO5_ERR_PLAYERS = -1,  /* nplayers not in 2..6                  */
    PLO5_ERR_BOARD   = -2,  /* nboard not 0, 3, 4 or 5               */
    PLO5_ERR_CARD    = -3,  /* card id out of range / bad random hand */
    PLO5_ERR_DUP     = -4,  /* duplicate card                        */
    PLO5_ERR_DECK    = -5,  /* not enough cards left in the deck     */
    PLO5_ERR_TRIALS  = -6,  /* trials == 0 and enumeration not possible */
    PLO5_ERR_ARG     = -7,  /* NULL pointer etc.                     */
    PLO5_ERR_RANKS   = -8,  /* rank table not loaded (plo5_ranks_load) */
    PLO5_ERR_IO      = -9,  /* rank file unreadable / unwritable     */
    PLO5_ERR_RANGE   = -10  /* range impossible given dead cards     */
};

/* player specification for plo5_equity2 */
enum { PLO5_P_FIXED = 0, PLO5_P_RANDOM = 1, PLO5_P_RANGE = 2 };

typedef struct {
    int    type;      /* PLO5_P_FIXED / PLO5_P_RANDOM / PLO5_P_RANGE */
    int    cards[5];  /* FIXED only */
    double lo, hi;    /* RANGE only: percentile band, 0..100.
                         0 = weakest hand, 100 = strongest.
                         "10..40" = 10th through 40th percentile.
                         The distribution is conditional on the board
                         passed to plo5_equity2: preflop it is the static
                         rank table; with a flop/turn/river it is the
                         board-conditional ranking (built automatically). */
} plo5_player;

typedef struct {
    double   equity[PLO5_MAX_PLAYERS]; /* pot share, in [0,1], sums to 1 */
    double   win[PLO5_MAX_PLAYERS];    /* fraction won outright          */
    double   tie[PLO5_MAX_PLAYERS];    /* fraction tied for best         */
    double   ci95[PLO5_MAX_PLAYERS];   /* 95% CI half-width on equity; 0 if exact */
    uint64_t samples;                  /* MC trials run, or boards enumerated */
    int      exact;                    /* 1 = exact enumeration, 0 = Monte Carlo */
} plo5_result;

/* Build lookup tables. Call once from the main thread before anything else.
 * (plo5_equity auto-inits as a safety net, but that path is not thread-safe.) */
void plo5_init(void);

/* Compute PLO5 equity.
 *   hands    nplayers*5 card ids; a player with all 5 entries == PLO5_RANDOM
 *            is dealt a random hand each trial
 *   board    0, 3, 4 or 5 known board cards (may be NULL if nboard == 0)
 *   dead     cards removed from the deck (may be NULL if ndead == 0)
 *   trials   Monte Carlo trial count
 *   max_enum enumerate exactly when the number of remaining runouts is
 *            <= max_enum (and no random players); pass 0 to force MC,
 *            UINT64_MAX to force enumeration whenever possible
 *   seed     PRNG seed (any value; same seed => same MC result)
 *   nthreads worker threads for MC (enumeration runs single-threaded)
 * Returns PLO5_OK or a PLO5_ERR_* code. */
int plo5_equity(const int *hands, int nplayers,
                const int *board, int nboard,
                const int *dead, int ndead,
                uint64_t trials, uint64_t max_enum,
                uint64_t seed, int nthreads,
                plo5_result *out);

/* Like plo5_equity but players may be fixed hands, fully random, or a
 * percentile band of the strength distribution (requires the rank table:
 * plo5_ranks_load or plo5_ranks_generate first). Range/random players are
 * Monte Carlo only; exact enumeration still applies when all are fixed. */
int plo5_equity2(const plo5_player *players, int nplayers,
                 const int *board, int nboard,
                 const int *dead, int ndead,
                 uint64_t trials, uint64_t max_enum,
                 uint64_t seed, int nthreads,
                 plo5_result *out);

/* Double-board split-pot PLO5 ("bomb pot"): two independent boards are
 * dealt; each board awards half the pot to its best hand(s). A player
 * best on both boards scoops the whole pot. equity = expected pot
 * fraction; win = scooped the whole pot alone; tie = won some share but
 * not all of it. Each board may have 0/3/4/5 known cards. Percentile
 * ranges use the preflop table when both boards are empty, or the
 * combined two-board distribution (built automatically) when BOTH boards
 * have at least a flop — one board set and the other empty is an error
 * for ranges. */
int plo5_equity_2b(const plo5_player *players, int nplayers,
                   const int *board1, int nboard1,
                   const int *board2, int nboard2,
                   const int *dead, int ndead,
                   uint64_t trials, uint64_t max_enum,
                   uint64_t seed, int nthreads,
                   plo5_result *out);

/* ---- hand-strength percentile table ---------------------------------
 * All C(52,5)=2,598,960 starting hands ranked by heads-up equity vs a
 * random hand (suit-isomorphic classes share a rank). The table is built
 * once by plo5_ranks_generate (a few minutes) and saved to disk. */

/* Build the table with trials_per_class MC trials per hand class
 * (25000 is a good default) and write it to path. Also leaves the table
 * loaded. progress (optional) is called periodically from the calling
 * thread with classes done / total. */
int plo5_ranks_generate(const char *path, uint32_t trials_per_class,
                        int nthreads, uint64_t seed,
                        void (*progress)(int done, int total, void *ud),
                        void *ud);

int plo5_ranks_load(const char *path);
int plo5_ranks_loaded(void);

/* percentile of a specific 5-card hand, 0..100 (100 = strongest);
 * returns -1.0 if no table is loaded */
double plo5_hand_percentile(const int cards[5]);

/* representative hand at a given percentile (0..100); PLO5_OK or error */
int plo5_percentile_hand(double pct, int out[5]);

/* ---- board-conditional rankings --------------------------------------
 * Rank every possible holding given a flop/turn/river:
 *   river: exact made-hand strength
 *   turn : average showdown percentile over all rivers (exact)
 *   flop : average showdown percentile over flop_runouts sampled
 *          turn+river runouts (default 100 when <= 0); deterministic
 * One board table is held at a time; plo5_equity2 (re)builds it
 * automatically when a RANGE player is used with a board. Not
 * thread-safe against concurrent queries while building. */
int plo5_board_ranks_build(const int *board, int nboard, int flop_runouts,
                           int nthreads,
                           void (*progress)(int done, int total, void *ud),
                           void *ud);

/* 1 if a board table is loaded (fills board_out/nboard_out), else 0 */
int plo5_board_ranks_state(int board_out[5], int *nboard_out);

/* percentile of a hand / representative hand for a percentile, in the
 * distribution conditional on the given board. nboard == 0 uses the
 * static preflop table; otherwise the board must match the built table
 * (plo5_board_ranks_build). Returns -1.0 / error code if unavailable. */
double plo5_hand_percentile_on(const int cards[5], const int *board, int nboard);
int    plo5_percentile_hand_on(double pct, const int *board, int nboard,
                               int out[5]);

/* Combined two-board distribution: holdings ranked by the sum of their
 * percentiles on each board. Same caching/threading rules as the
 * single-board table (separate slot: single- and two-board tables can
 * coexist). Board order does not matter. */
int plo5_board_ranks_build2(const int *b1, int nb1, const int *b2, int nb2,
                            int flop_runouts, int nthreads,
                            void (*progress)(int done, int total, void *ud),
                            void *ud);
int plo5_board_ranks_state2(int b1_out[5], int *nb1_out,
                            int b2_out[5], int *nb2_out);
double plo5_hand_percentile_2b(const int cards[5], const int *b1, int nb1,
                               const int *b2, int nb2);
int    plo5_percentile_hand_2b(double pct, const int *b1, int nb1,
                               const int *b2, int nb2, int out[5]);

/* Evaluate a 5-card poker hand; higher value = stronger hand.
 * Category is (value >> 24): 0 high card, 1 pair, 2 two pair, 3 trips,
 * 4 straight, 5 flush, 6 full house, 7 quads, 8 straight flush. */
uint32_t plo5_eval5(const int c[5]);

/* "Ah" -> card id, or -1 on error. Case-insensitive. */
int plo5_parse_card(const char *s);

/* card id -> "Ah" (out must hold 3 bytes) */
void plo5_card_str(int id, char out[3]);

#ifdef __cplusplus
}
#endif
#endif /* PLO5_H */
