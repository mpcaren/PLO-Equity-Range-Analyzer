/* cfg.h — plo5config.ini reader (written once per machine by plo5setup).
 *
 * The ini lives next to the executables and holds machine-specific
 * settings: thread count, calibrated Monte Carlo trial presets, and
 * optional server-hosting defaults. Every field is optional; apps fall
 * back to runtime auto-detection when the file or a key is missing, so
 * the tools work with no setup at all — plo5setup just makes the
 * defaults fit the machine.
 */
#ifndef PLO5_CFG_H
#define PLO5_CFG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      threads;     /* 0 = auto-detect            */
    int      port;        /* 0 = app default            */
    int      lan;         /* -1 = unset, else 0/1       */
    char     password[128];
    uint64_t trials[3];   /* Fast/Balanced/Precise; 0 = app default */
} plo5_cfg;

/* fill with "everything unset" */
void plo5_cfg_init(plo5_cfg *c);

/* parse `key=value` lines ('#' comments); returns 1 if the file was
 * read, 0 if it does not exist / cannot be opened */
int plo5_cfg_load(plo5_cfg *c, const char *path);

/* build "<dir of exe>\plo5config.ini" into out (portable helper);
 * falls back to just "plo5config.ini" (cwd) when the exe path is
 * unavailable */
void plo5_cfg_default_path(char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* PLO5_CFG_H */
