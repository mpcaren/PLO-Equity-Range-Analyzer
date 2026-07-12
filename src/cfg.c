/* cfg.c — plo5config.ini reader. See cfg.h. */
#include "cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void plo5_cfg_init(plo5_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->lan = -1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int plo5_cfg_load(plo5_cfg *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p), *val = trim(eq + 1);
        if      (!strcmp(key, "threads"))          c->threads = atoi(val);
        else if (!strcmp(key, "port"))             c->port = atoi(val);
        else if (!strcmp(key, "lan"))              c->lan = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "password"))
            snprintf(c->password, sizeof c->password, "%s", val);
        else if (!strcmp(key, "trials_fast"))      c->trials[0] = (uint64_t)atof(val);
        else if (!strcmp(key, "trials_balanced"))  c->trials[1] = (uint64_t)atof(val);
        else if (!strcmp(key, "trials_precise"))   c->trials[2] = (uint64_t)atof(val);
        /* unknown keys are ignored so the format can grow */
    }
    fclose(f);
    return 1;
}

void plo5_cfg_default_path(char *out, int cap)
{
#ifdef _WIN32
    char exe[1024];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            *slash = 0;
            snprintf(out, (size_t)cap, "%s\\plo5config.ini", exe);
            return;
        }
    }
#endif
    snprintf(out, (size_t)cap, "plo5config.ini");
}
