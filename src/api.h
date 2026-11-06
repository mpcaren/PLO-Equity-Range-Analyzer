/* api.h — the engine's JSON API, independent of any transport.
 *
 * Shared by plo5web (HTTP, Windows) and plo5svc (stdin/stdout, any OS,
 * used by the Electron desktop app). A request is an endpoint name plus
 * a URL-encoded query string, exactly as in the HTTP API:
 *
 *   plo5_api_call("/api/equity", "players=AhAd..,random&board=Kh7d2s", &sb, ..)
 *
 * The response body (always a JSON object; {"error":".."} on failure) is
 * appended to the string buffer and an HTTP-style status code returned.
 *
 * Not thread-safe: callers must serialize calls (the engine's board-
 * ranking caches assume single-threaded mutation).
 */
#ifndef PLO5_API_H
#define PLO5_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* growable string buffer */
typedef struct { char *buf; size_t len, cap; } plo5_sb;

void plo5_sb_init(plo5_sb *s);
void plo5_sb_free(plo5_sb *s);
void plo5_sb_put(plo5_sb *s, const char *p, size_t n);
void plo5_sb_printf(plo5_sb *s, const char *fmt, ...);

/* optional progress reporting for slow steps (board ranking builds);
 * `what` is a short label such as "flop ranking" */
typedef void (*plo5_api_progress_fn)(const char *what, int done, int total,
                                     void *ud);

/* threads used for engine work and the Fast/Balanced/Precise trial
 * presets reported by /api/status (NULL = built-in defaults) */
void plo5_api_init(int nthreads, const uint64_t presets[3]);

/* run one request; returns 200, 400 (body is {"error":..}) or 404 */
int plo5_api_call(const char *endpoint, const char *query, plo5_sb *out,
                  plo5_api_progress_fn progress, void *ud);

/* url-decoded value of key from a query string into out; 0 if missing */
int plo5_api_qget(const char *query, const char *key, char *out, size_t cap);

/* human-readable message for a PLO5_ERR_* code */
const char *plo5_api_err_str(int rc);

/* milliseconds from a monotonic clock / a fresh nonzero seed */
double   plo5_api_now_ms(void);
uint64_t plo5_api_seed(void);

/* online CPU count (>= 1) */
int plo5_api_ncpu(void);

#ifdef __cplusplus
}
#endif
#endif /* PLO5_API_H */
