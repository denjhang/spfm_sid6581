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

/* Chip type codes used in the extra header's clock table. They occupy the gap
 * between libvgm's last device (0x29 MIKEY) and 0xFF, so a VGM reader that
 * ever grew SID support would not collide with an existing ID. */
#define SIDVGM_CHIP_6581 0x50
#define SIDVGM_CHIP_8580 0x51
#define SIDVGM_CHIP_ANY  0x52

/* Command opcodes. Deliberately not VGM's 0x50-0x5F block: those are taken by
 * SN76489/YM writes, and a file that looks half-playable on a VGM player is a
 * worse outcome than one that is obviously a different format. */
#define SIDVGM_CMD_WRITE0 0x69
#define SIDVGM_CMD_WRITE1 0x6A
#define SIDVGM_CMD_WRITE2 0x6B
#define SIDVGM_CMD_WAIT   0x61
#define SIDVGM_CMD_DATA   0x67
#define SIDVGM_CMD_END    0x66

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