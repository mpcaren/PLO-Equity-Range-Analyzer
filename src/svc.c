/* svc.c — plo5svc: the PLO5 engine as a stdin/stdout service.
 *
 * Used by the Electron desktop app (electron/), which spawns it as a
 * child process: no network port, no firewall prompt, any OS. Requests
 * are processed one at a time, in order.
 *
 * Request  (one line on stdin, tab-separated):
 *     <id> TAB <endpoint> TAB <url-encoded query>
 *   endpoint is any /api/... route of api.c (same parameters as the HTTP
 *   API served by plo5web), or one of:
 *     /svc/info       -> {"ranks":bool,"rankPath":"..","ncpu":N}
 *     /svc/genranks   build the preflop rank table (trials=N optional),
 *                     save it to the --ranks path and load it
 *
 * Responses (one JSON object per line on stdout):
 *     {"id":N,"status":200,"body":{...}}
 *     {"id":N,"progress":{"what":"flop ranking","done":40,"total":100}}
 *   and once at startup: {"event":"ready","ranks":bool}
 *
 * The service exits when stdin is closed.
 *
 * Usage: plo5svc [--ranks FILE] [--threads N] [--config FILE]
 *   --ranks   rank table to load (default: plo5rank.bin next to the exe)
 *   --threads engine threads (default: config file, else all cores)
 *   --config  plo5config.ini to read (default: next to the exe)
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plo5.h"
#include "api.h"
#include "cfg.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static char g_rankpath[2048] = "plo5rank.bin";
static int  g_threads = 1;

/* directory of the running executable, or "" */
static void exe_dir(char *out, size_t cap)
{
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    char *s = strrchr(out, '\\');
    if (s) *s = 0; else out[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
    char *s = strrchr(out, '/');
    if (s) *s = 0; else out[0] = 0;
#endif
}

/* JSON string escape for paths */
static void put_json_str(plo5_sb *sb, const char *s)
{
    plo5_sb_put(sb, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; plo5_sb_put(sb, e, 2); }
        else if (c < 0x20) plo5_sb_printf(sb, "\\u%04x", c);
        else plo5_sb_put(sb, (const char *)&c, 1);
    }
    plo5_sb_put(sb, "\"", 1);
}

/* ---- output: every line is written whole and flushed ---- */

static long g_cur_id;

static void emit_progress(const char *what, int done, int total, void *ud)
{
    (void)ud;
    printf("{\"id\":%ld,\"progress\":{\"what\":\"%s\",\"done\":%d,\"total\":%d}}\n",
           g_cur_id, what, done, total);
    fflush(stdout);
}

static void gen_progress(int done, int total, void *ud)
{
    emit_progress("preflop rank table", done, total, ud);
}

static void emit_response(long id, int status, const plo5_sb *body)
{
    printf("{\"id\":%ld,\"status\":%d,\"body\":%s}\n", id, status,
           body->buf && body->len ? body->buf : "{}");
    fflush(stdout);
}

/* ---- service-only endpoints ---- */

static int svc_info(plo5_sb *sb)
{
    plo5_sb_printf(sb, "{\"ranks\":%s,\"ncpu\":%d,\"rankPath\":",
                   plo5_ranks_loaded() ? "true" : "false", g_threads);
    put_json_str(sb, g_rankpath);
    plo5_sb_put(sb, "}", 1);
    return 200;
}

static int svc_genranks(const char *q, plo5_sb *sb)
{
    char v[32];
    uint32_t trials = 25000;
    if (plo5_api_qget(q, "trials", v, sizeof v) && atoi(v) > 0)
        trials = (uint32_t)atoi(v);

    /* write to a temp file and move it into place, so a cancelled or
     * crashed build never leaves a truncated table behind */
    char tmp[2100];
    snprintf(tmp, sizeof tmp, "%s.part", g_rankpath);
    double t0 = plo5_api_now_ms();
    int rc = plo5_ranks_generate(tmp, trials, g_threads, 20260704,
                                 gen_progress, NULL);
    if (rc != PLO5_OK) {
        remove(tmp);
        plo5_sb_printf(sb, "{\"error\":\"%s\"}", plo5_api_err_str(rc));
        return 400;
    }
    remove(g_rankpath);
    if (rename(tmp, g_rankpath) != 0) {
        plo5_sb_printf(sb, "{\"error\":\"%s\"}", plo5_api_err_str(PLO5_ERR_IO));
        return 400;
    }
    rc = plo5_ranks_load(g_rankpath);
    plo5_sb_printf(sb, "{\"ranks\":%s,\"seconds\":%.1f}",
                   rc == PLO5_OK ? "true" : "false",
                   (plo5_api_now_ms() - t0) / 1000.0);
    return 200;
}

/* ---- request loop ---- */

static void handle_line(char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) return;

    char *t1 = strchr(line, '\t');
    if (!t1) return;                  /* not a request: ignore */
    *t1 = 0;
    long id = strtol(line, NULL, 10);
    char *endpoint = t1 + 1;
    char *t2 = strchr(endpoint, '\t');
    char *query = "";
    if (t2) { *t2 = 0; query = t2 + 1; }

    g_cur_id = id;
    plo5_sb sb;
    plo5_sb_init(&sb);
    int status;
    if (!strcmp(endpoint, "/svc/info"))          status = svc_info(&sb);
    else if (!strcmp(endpoint, "/svc/genranks")) status = svc_genranks(query, &sb);
    else status = plo5_api_call(endpoint, query, &sb, emit_progress, NULL);
    emit_response(id, status, &sb);
    plo5_sb_free(&sb);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   /* \n line endings, no \r */
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    plo5_init();

    const char *ranks_arg = NULL, *cfg_arg = NULL;
    int threads_arg = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ranks") && i + 1 < argc) ranks_arg = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_arg = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: plo5svc [--ranks FILE] [--threads N] [--config FILE]\n"
                            "  reads requests from stdin; see src/svc.c for the protocol\n");
            return 0;
        }
    }

    char dir[2048];
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    plo5_cfg cfg;
    plo5_cfg_init(&cfg);
    char inipath[2100];
    if (cfg_arg) snprintf(inipath, sizeof inipath, "%s", cfg_arg);
    else if (dir[0]) snprintf(inipath, sizeof inipath, "%s%cplo5config.ini", dir, sep);
    else snprintf(inipath, sizeof inipath, "plo5config.ini");
    plo5_cfg_load(&cfg, inipath);

    int threads = threads_arg > 0 ? threads_arg : cfg.threads;
    plo5_api_init(threads, cfg.trials);
    g_threads = threads > 0 ? threads : plo5_api_ncpu();

    if (ranks_arg) snprintf(g_rankpath, sizeof g_rankpath, "%s", ranks_arg);
    else if (dir[0]) snprintf(g_rankpath, sizeof g_rankpath, "%s%cplo5rank.bin", dir, sep);
    plo5_ranks_load(g_rankpath);   /* missing is fine: /svc/genranks builds it */

    printf("{\"event\":\"ready\",\"ranks\":%s}\n", plo5_ranks_loaded() ? "true" : "false");
    fflush(stdout);

    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        if (n == sizeof line - 1 && line[n - 1] != '\n') {
            /* overlong line: drain the rest and ignore it */
            int ch;
            while ((ch = getchar()) != EOF && ch != '\n') {}
            continue;
        }
        handle_line(line);
    }
    return 0;
}
