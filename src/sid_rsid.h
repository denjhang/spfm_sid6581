/*
 * sid_rsid.h - PSID/RSID -> SID register stream, in plain C.
 *
 * A .sid file is 6502 machine code plus a header; the music only exists as
 * register writes made by the init/play routines. This module runs those
 * routines on an embedded 6502 (fake6502 core, with the VIC raster and the two
 * CIA timers that RSID files use for their interrupt clock), records every
 * write to $D400-$D41F, and hands back a timestamped register stream.
 *
 * The stream is what the SPFM driver can actually replay: the SID has no
 * concept of notes, only registers, so "playing a .sid" means pushing these
 * writes out at the right wall-clock times.
 *
 * The 6502 core and the CIA/VIC models come from the STC32G chiptune
 * project (tools/sid_extract.c), where they were already used to render SID
 * files; the code is reused rather than rewritten so both projects stay
 * comparable.
 */
#ifndef SID_RSID_H
#define SID_RSID_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One captured register write. t_ms is measured from the start of playback,
 * where 0 is the moment the init routine's writes are done. */
typedef struct {
    double  t_ms;
    uint8_t reg;        /* 0x00 .. 0x1F */
    uint8_t val;
} sid_event_t;

typedef struct {
    char     name[33];
    char     author[33];
    char     released[33];
    int      is_rsid;
    int      pal;               /* 1 = PAL (50.125 Hz), 0 = NTSC (60 Hz) */
    double   clock_hz;          /* 985248 PAL / 1022730 NTSC */
    double   vsync_hz;
    uint16_t load_addr;
    uint16_t init_addr;
    uint16_t play_addr;         /* 0 for RSID */
    uint16_t n_songs;
    uint16_t start_song;        /* 1-based */
    uint32_t speed;
    uint16_t flags;
} sid_song_info_t;

/* Header-only probe: fills info without running any 6502 code, so a playlist
 * can show song titles and subsong counts instantly.
 * Returns 0 on success, -1 on error. */
int sid_song_probe(const char *path, sid_song_info_t *info);

/* Run init + play for `seconds` and return the captured writes, sorted by
 * time. `subsong` is 0-based; pass -1 for the file's default song.
 * `events` is malloc'd and must be released with sid_song_free().
 * Returns 0 on success, -1 on error (err gets a human-readable reason). */
int sid_song_extract(const char *path, int subsong, double seconds,
                     sid_event_t **events, size_t *count,
                     sid_song_info_t *info, char *err, size_t errlen);

void sid_song_free(sid_event_t *events);

/* ---- .sidreg text cache --------------------------------------------- *
 * The 6502 emulation costs about a second per minute of music, so a
 * playlist wants to pay that once. Format is the one the STC32G tools use,
 * one write per line:
 *
 *     <time_ms> <reg_hex> <val_hex>
 */
int sid_sidreg_write(const char *path, const sid_event_t *events, size_t count);
int sid_sidreg_read(const char *path, sid_event_t **events, size_t *count);
int sid_sidreg_exists(const char *path);

/* Build the conventional cache name for a song:
 *   <cache_dir>/<basename>_s<subsong>_d<seconds>.sidreg
 * Returns 0 if the result fit, -1 if it had to be truncated. */
int sid_sidreg_cache_name(char *buf, size_t len, const char *cache_dir,
                          const char *sid_path, int subsong, double seconds);

#ifdef __cplusplus
}
#endif

#endif /* SID_RSID_H */
