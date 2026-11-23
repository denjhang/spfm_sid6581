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

/* ---- live streaming (real hardware) --------------------------------- *
 * The hardware player cannot render audio, it needs the register writes as
 * they happen. sid_lsfp_extract() pays for the whole song up front, which is
 * wrong for a tune that has no end: a .sid plays forever. This walks
 * libsidplayfp one frame at a time and hands back only that frame's writes, so
 * the emulation of frame N+1 hides inside the ~20 ms frame N is playing.
 *
 * Same 6502/VIC/CIA as the extractor, so $D418 drum writes and CIA timer
 * cadence are the reference ones rather than the old fake6502 approximation.
 *
 *   sid_lsfp_stream_t *s = sid_lsfp_stream_open(path, subsong, err, sizeof err);
 *   while (sid_lsfp_stream_next(s, &ev, &n) == 0) {
 *       ... emit ev[0..n) at their t_ms, then wait for the frame to end ...
 *   }
 *
 * t_ms is absolute from the first play frame for the whole session. Only one
 * stream is open at a time in practice, but the API holds no global state. */

typedef struct sid_lsfp_stream sid_lsfp_stream_t;

sid_lsfp_stream_t *sid_lsfp_stream_open(const char *path, int subsong,
                                        char *err, size_t errlen);

/* Advance one frame (except the first call, which also returns the init
 * writes and may run a couple of frames to find where the init burst ends).
 * Returns 0 on success, -1 when the tune ended or the emulator failed.
 * *ev points into a buffer owned by the stream and is valid until the next
 * call; *n may be 0 for a frame that writes no registers. */
int sid_lsfp_stream_next(sid_lsfp_stream_t *s,
                         const sid_event_t **ev, size_t *n);

/* Length of one frame in ms (19.96 PAL / 16.67 NTSC). */
double sid_lsfp_stream_frame_ms(const sid_lsfp_stream_t *s);

void sid_lsfp_stream_close(sid_lsfp_stream_t *s);

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
