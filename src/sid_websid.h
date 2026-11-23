/*
 * sid_websid.h - adapters between this project and the vendored WebSid
 * emulator (third_party/websid, upstream wothke/websid, unmodified).
 *
 * Two independent things can be taken from WebSid:
 *
 *   1. the register-stream recorder  (sid_websid_extract), which replaces the
 *      hand-written 6502 in sid_rsid.c. It is more accurate because WebSid's
 *      CPU/VIC/CIA timing is cycle-exact, so intra-frame timestamps are right.
 *
 *   2. the audio renderer            (sid_websid_render_*), which replaces the
 *      hand-written sidmodel.c when auditioning a .sidvgm/.sidreg stream.
 *
 * Both are optional: the native implementations stay available and remain the
 * default until the WebSid path has been compared against them.
 *
 * Everything here is C so the two CLI tools (which are C) can call it.
 */
#ifndef SID_WEBSID_H
#define SID_WEBSID_H

#include <stdint.h>
#include <stddef.h>

#include "sid_rsid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = this build has the WebSid adapter linked in. */
int sid_websid_available(void);

/* Run the song on WebSid and return the captured $D400-$D41F writes.
 * Same contract as sid_song_extract(): `subsong` is 0-based (-1 = the file's
 * default), `events` is malloc'd and released with sid_song_free().
 * `info` supplies the clock (PAL/NTSC), which is also how cycles map to ms. */
int sid_websid_extract(const char *path, int subsong, double seconds,
                       sid_event_t **events, size_t *count,
                       sid_song_info_t *info, char *err, size_t errlen);

/* ---- audio renderer, same shape as sidmodel.h ------------------------ */

typedef struct sid_websid_render sid_websid_render_t;

sid_websid_render_t *sid_websid_render_new(double clock_hz, int model8580,
                                           double sample_rate);
void sid_websid_render_free(sid_websid_render_t *r);
void sid_websid_render_reset(sid_websid_render_t *r);
void sid_websid_render_write(sid_websid_render_t *r, uint8_t reg, uint8_t val);
void sid_websid_render_render(sid_websid_render_t *r, short *buf, size_t frames);
void sid_websid_render_set_filter(sid_websid_render_t *r, int on);

#ifdef __cplusplus
}
#endif

#endif /* SID_WEBSID_H */
