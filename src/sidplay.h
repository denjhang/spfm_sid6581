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

/* Write protocol, i.e. how one register write becomes bus cycles.
 *
 *   "2step" (default) - two separate Path B commands, gap under host control:
 *                       [slot,0x80,addr] then [slot,0x81,data]
 *   "1step"           - one Path A command, firmware emits both bus cycles
 *                       itself: [slot,0x00,addr,data]
 *
 * These are the two settings worth judging by ear; the exotic variants
 * (split, nop-paced, path-a-soft) stay reachable through --write-mode.
 * Returns 0 if the name was recognised, -1 otherwise. */
int         sidplay_set_proto(const char *name);
const char *sidplay_proto_name(void);

/* Tell the player that the write mode was already chosen elsewhere (sidtest's
 * --write-mode calls sid_set_write_mode directly), so it must not fall back to
 * its own playback default. */
void sidplay_set_proto_explicit(void);

/* Each returns 0 on success, 1 on a usage or hardware error. */
int sidplay_do_play(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_extract(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_info(int argc, char **argv, const sidplay_opts_t *opts);
int sidplay_do_list(int argc, char **argv, const sidplay_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* SIDPLAY_H */
