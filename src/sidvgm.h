/*
 * sidvgm.h - SID register stream in a VGM-shaped binary container.
 *
 * The format is specified in docs/sidvgm文件规范.md. In one line: the byte
 * layout, the relative-offset rule, the wait commands and the GD3 tag follow
 * libvgm's VGM reader exactly, and only the chip commands are ours (0x69/0x6A/
 * 0x6B for the three SID sockets). libvgm itself has no SID device - the
 * DEVID_* list stops at 0x29 - so this is a sibling format, not a VGM variant
 * that libvgm can play.
 *
 * Why bother replacing .sidreg: .sidreg is text with a float millisecond
 * column. It cannot say which clock the tune runs at, which chip model it
 * wants, how many chips it uses, what the tune is called, or where a loop
 * begins; and %.4f milliseconds carries about 0.1 us of noise per event.
 * sidvgm answers all of that in a fixed header and keeps time in CPU cycles,
 * so a stream is exactly round-trippable.
 */
#ifndef SIDVGM_H
#define SIDVGM_H

#include <stdio.h>
#include <stddef.h>

#include "sid_rsid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header size up to the extra-header pointer. Everything past 0xBC is either
 * the extra header or command data. */
#define SIDVGM_MAIN_HDR 0xC0

/* Chip type codes for the three SID sockets. libvgm's DEVID_* space runs
 * 0x00..0x29 with _CHIP_COUNT = 0x2A, so anything above that is free and is
 * resolved by the branch player that understands SID. One code per socket so
 * the extra header's per-type clock lookup stays unambiguous. */
#define SIDVGM_CHIP_SID0 0x65
#define SIDVGM_CHIP_SID1 0x66
#define SIDVGM_CHIP_SID2 0x67

/* Command opcodes. VGM's 0x41..0x4E are "unknown, length 3" slots - exactly
 * the shape a SID write needs (opcode, reg, data) - so the three writes take
 * the first three of them. A stock libvgm player skips them by their declared
 * length instead of aborting, which is the least-surprising behaviour for a
 * format it does not know. */
#define SIDVGM_CMD_WRITE0 0x41
#define SIDVGM_CMD_WRITE1 0x42
#define SIDVGM_CMD_WRITE2 0x43
#define SIDVGM_CMD_WAIT   0x61
#define SIDVGM_CMD_DATA   0x67
#define SIDVGM_CMD_END    0x66

/* VGM's tick base. Not the recording sample rate: every wait count in the
 * file is in units of 1/44100 s, and 0x24 is only a region tag (50 PAL,
 * 60 NTSC, 0 = no rate scaling). libvgm hardcodes this: vgmplayer.cpp's
 * RefreshTSRates sets _tsDiv = 44100 unconditionally. */
#define SIDVGM_TICK_HZ 44100

/* Write a stream. `info` may be NULL, in which case the header carries no
 * chip model, no subsong count and a single 6581 chip. `comment` may be NULL.
 * Returns 0 on success. */
int sidvgm_write(const char *path, const sid_event_t *ev, size_t n,
                 const sid_song_info_t *info, const char *comment);

/* Read a stream. `ev` is malloc'd and belongs to the caller (free() or
 * sid_song_free). `info` is filled with what the header states; fields the
 * format cannot carry (load/init/play addresses) are left at 0, which is
 * exactly the information a rendered stream does not need.
 * Returns 0 on success, -1 if the file is not a sidvgm or is malformed. */
int sidvgm_read(const char *path, sid_event_t **ev, size_t *n,
                sid_song_info_t *info);

/* 1 if the file starts with the "Svgm" magic. Cheap enough to call before
 * committing to a reader. */
int sidvgm_is_file(const char *path);

/* Human-readable dump of the header and the first `max_cmds` commands
 * (0 = all). Used by sidextract --dump; prints to `out`. */
int sidvgm_dump(FILE *out, const char *path, unsigned max_cmds);

#ifdef __cplusplus
}
#endif

#endif /* SIDVGM_H */