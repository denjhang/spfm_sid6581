/*
 * sidplay.h - play .sid files on the SPFM SID.
 *
 * A .sid file contains 6502 code, not notes: the only way to play it on real
 * hardware is to run the code once, record every write it makes to $D400-$D41F
 * together with the moment it happened, and then replay that stream at the
 * same wall-clock pace. sid_rsid.c does the first half (emulation), sidplay.c
 * does the second (scheduling) plus the CLI around it.
 *
 * The player also accepts a pre-rendered .sidreg text file. That makes it
 * possible to tweak a stream by hand, or to reuse a cache produced by the
 * STC32G tools, without touching the emulator.
 */
#ifndef SIDPLAY_H
#define SIDPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t slot;         /* SPFM slot, normally SID_DEFAULT_SLOT */
    int     device;       /* FTDI device index */
    int     dry_run;      /* 1 = do not touch hardware */
    int     write_mode;   /* 0 = keep the driver default, otherwise a
                           * sid_write_mode_t value */
} sidplay_opts_t;

/* Each returns 0 on success, 1 on a usage or hardware error. */
int sidplay_do_play(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_extract(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_info(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_list(int argc, char **argv, const sidplay_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* SIDPLAY_H */
