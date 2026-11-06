/* web.c — local web UI server for the PLO5 equity engine.
 *
 * Serves the React app in web/ (next to the exe) and exposes the engine
 * as a small JSON-over-HTTP API. Binds 127.0.0.1 by default; with --lan
 * it listens on all interfaces and requires a password (HTTP Basic auth)
 * from any client that is not this machine. Engine work is serialized,
 * which also keeps the board-ranking caches race-free.
 *
 * The API itself lives in api.c (portable C); this file is only HTTP,
 * auth and static files.
 *
 * Build (MSVC):  cl /O2 /std:c11 /utf-8 /Isrc src\web.c src\api.c src\spr.c src\cfg.c src\plo5.c /Fe:plo5web.exe ws2_32.lib shell32.lib
 * Build (gcc) :  gcc -O3 -std=c11 -Isrc -o plo5web src/web.c src/api.c src/spr.c src/cfg.c src/plo5.c -lws2_32
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
#include "api.h"
#include "cfg.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

static char g_password[128] = "";   /* required from non-local clients */

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

static void respond_json(SOCKET c, int code, plo5_sb *sb)
{
    respond(c, code, "application/json; charset=utf-8", sb->buf ? sb->buf : "{}",
            sb->len);
    plo5_sb_free(sb);
}

/* ------------------------------------------------------------------ */
/* Password auth for non-local clients (HTTP Basic)                    */
/* ------------------------------------------------------------------ */

static int b64_decode(const char *in, char *out, int cap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, bits = 0, n = 0;
    for (; *in && *in != '='; in++) {
        const char *p = strchr(tbl, *in);
        if (!p) return -1;
        val = val << 6 | (int)(p - tbl);
        bits += 6;
        if (bits >= 8) {
            if (n + 1 >= cap) return -1;
            out[n++] = (char)(val >> (bits - 8) & 0xFF);
            bits -= 8;
        }
    }
    out[n] = 0;
    return n;
}

/* constant-time compare so the password can't be probed byte by byte */
static int pw_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
    return diff == 0;
}

/* Authorization: Basic base64(user:password) — any username accepted */
static int authorized(const char *req)
{
    const char *p = req;
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (_strnicmp(p, "Authorization:", 14) != 0) continue;
        p += 14;
        while (*p == ' ') p++;
        if (_strnicmp(p, "Basic", 5) != 0) return 0;
        p += 5;
        while (*p == ' ') p++;
        char tok[256];
        int i = 0;
        while (*p && *p != '\r' && *p != '\n' && i < (int)sizeof tok - 1)
            tok[i++] = *p++;
        tok[i] = 0;
        char dec[256];
        if (b64_decode(tok, dec, sizeof dec) < 0) return 0;
        const char *colon = strchr(dec, ':');
        return pw_equal(colon ? colon + 1 : dec, g_password);
    }
    return 0;
}

static void respond_401(SOCKET c)
{
    static const char body[] = "password required";
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"PLO5 Equity Lab\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        sizeof body - 1);
    send_all(c, hdr, (size_t)hn);
    send_all(c, body, sizeof body - 1);
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
/* Server loop                                                         */
/* ------------------------------------------------------------------ */

/* Socket I/O runs on one thread per connection (browsers open idle
 * speculative connections that must not block the server), but all
 * engine work is serialized — the board-ranking caches assume single-
 * threaded mutation. */
static CRITICAL_SECTION g_engine_cs;

static void handle(SOCKET c, int is_local)
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
    if (!is_local && !authorized(req)) {
        respond_401(c);
        return;
    }
    char *qs = strchr(url, '?');
    if (qs) *qs++ = 0;
    else qs = url + strlen(url);

    if (strncmp(url, "/api/", 5) == 0) {
        plo5_sb sb;
        plo5_sb_init(&sb);
        EnterCriticalSection(&g_engine_cs);
        int code = plo5_api_call(url, qs, &sb, NULL, NULL);
        LeaveCriticalSection(&g_engine_cs);
        respond_json(c, code, &sb);
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
    struct sockaddr_in pa;
    int pal = (int)sizeof pa, is_local = 0;
    if (getpeername(c, (struct sockaddr *)&pa, &pal) == 0)
        is_local = (ntohl(pa.sin_addr.s_addr) >> 24) == 127;   /* 127/8 */
    handle(c, is_local);
    shutdown(c, SD_SEND);
    closesocket(c);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* password banner must not sit in a buffer */
    plo5_init();

    /* per-machine config written by plo5setup; missing file = defaults */
    plo5_cfg cfg;
    plo5_cfg_init(&cfg);
    char inipath[1200];
    plo5_cfg_default_path(inipath, sizeof inipath);
    int have_cfg = plo5_cfg_load(&cfg, inipath);

    plo5_api_init(cfg.threads, cfg.trials);

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

    /* config supplies the defaults, command-line arguments override */
    int open_browser = 1, port = cfg.port > 0 ? cfg.port : 8722;
    int lan = cfg.lan == 1;
    if (cfg.password[0]) {
        snprintf(g_password, sizeof g_password, "%s", cfg.password);
        lan = 1;
    }
    if (have_cfg) printf("config loaded from %s\n", inipath);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-browser") == 0) open_browser = 0;
        else if (strcmp(argv[i], "--lan") == 0) lan = 1;
        else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            strncpy(g_password, argv[++i], sizeof g_password - 1);
            lan = 1;   /* a password only makes sense with remote access */
        }
        else if (atoi(argv[i]) > 0) port = atoi(argv[i]);
    }

    if (lan && !g_password[0]) {
        /* no password given - generate one so the LAN port is never open */
        static const char al[] =
            "abcdefghjkmnpqrstuvwxyzACDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint64_t s = plo5_api_seed();
        for (int i = 0; i < 10; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            g_password[i] = al[s % (sizeof al - 1)];
        }
        g_password[10] = 0;
    }

    /* localhost only unless --lan / --password was given */
    addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
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

    if (lan) {
        printf("\nLAN access is ON - other devices can connect at:\n");
        char host[256];
        if (gethostname(host, sizeof host) == 0) {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, NULL, &hints, &res) == 0) {
                for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
                    struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
                    char ip[64];
                    if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip))
                        printf("    http://%s:%d\n", ip, port);
                }
                freeaddrinfo(res);
            }
        }
        printf("password: %s   (leave the username blank)\n"
               "This machine needs no password; everyone else is asked for one.\n"
               "If other devices cannot connect, allow plo5web.exe through the\n"
               "Windows Firewall when prompted.\n\n", g_password);
    }

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
