/*
 * sid_lsfp.h - adapters between this project and libsidplayfp + reSIDfp.
 *
 * libsidplayfp is the recording reference (6502 + VIC + CIA exactly as the
 * reference players run them) and reSIDfp is the audio reference (the same
 * cycle-exact SID the reference players use). Both are the untouched upstream
 * builds in third_party/local; nothing here reimplements either.
 *
 *   1. sid_lsfp_extract()   - run the song on libsidplayfp's player and return
 *      the $D400-$D41F writes it makes, timestamped from its own scheduler.
 *      The SID device is a custom sidemu that records instead of sounding.
 *
 *   2. sid_lsfp_render_*    - turn a register stream into samples with
 *      reSIDfp, i.e. the same core `build\lsfpref.exe` uses.
 *
 * Both are C so the CLI tools (which are C) can call them.
 */
#ifndef SID_LSFP_H
#define SID_LSFP_H

#include <stdint.h>
#include <stddef.h>

#include "sid_rsid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = this build has the libsidplayfp adapter linked in. */
int sid_lsfp_available(void);

/* Run the song through libsidplayfp and return the captured register writes.
 * Same contract as sid_song_extract(): `subsong` is 0-based (-1 = the file's
 * default), `events` is malloc'd and released with sid_song_free().
 *
 * Timestamps come from libsidplayfp's event scheduler (PHI1 clock), converted
 * with the tune's own clock, and are measured from the end of the first
 * emulated frame. Everything the driver's init routine wrote lands there
 * (t=0), which is the same convention the other two recorders use. */
int sid_lsfp_extract(const char *path, int subsong, double seconds,
                     sid_event_t **events, size_t *count,
                     sid_song_info_t *info, char *err, size_t errlen);

/* ---- audio renderer, same shape as sidmodel.h ------------------------ */

typedef struct sid_lsfp_render sid_lsfp_render_t;

sid_lsfp_render_t *sid_lsfp_render_new(double clock_hz, int model8580,
                                       double sample_rate);
void sid_lsfp_render_free(sid_lsfp_render_t *r);
void sid_lsfp_render_reset(sid_lsfp_render_t *r);
void sid_lsfp_render_write(sid_lsfp_render_t *r, uint8_t reg, uint8_t val);
void sid_lsfp_render_render(sid_lsfp_render_t *r, short *buf, size_t frames);
void sid_lsfp_render_set_filter(sid_lsfp_render_t *r, int on);

/* ---- direct player --------------------------------------------------- *
 * A .sid can also be played straight through libsidplayfp + reSIDfp, without
 * recording the register stream first. That is what build/lsfpref.exe does, so
 * the audio is the reference audio: no replay step, and SID register reads
 * ($D41B/$D41C noise tricks) still work because the same chip instance that is
 * written is the one being read.
 *
 * Use this for listening; use sid_lsfp_extract() when the goal is a register
 * stream for the real hardware. */

typedef struct sid_lsfp_player sid_lsfp_player_t;

sid_lsfp_player_t *sid_lsfp_player_open(const char *path, int subsong,
                                        double sample_rate, int model8580,
                                        char *err, size_t errlen);
void sid_lsfp_player_close(sid_lsfp_player_t *p);

/* Render exactly `frames` mono samples. Returns the number produced (0 at the
 * end of the tune or on error). */
size_t sid_lsfp_player_render(sid_lsfp_player_t *p, short *buf, size_t frames);

/* Length the tune has produced so far, in seconds. */
double sid_lsfp_player_elapsed(const sid_lsfp_player_t *p);

#ifdef __cplusplus
}
#endif

#endif /* SID_LSFP_H */
