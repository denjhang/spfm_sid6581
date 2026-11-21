/*
 * sidmeta.h - everything a .sid file does not carry itself.
 *
 * The PSID header has a title, an author and a subsong count. It does NOT have
 * a song length or a per-subsong name - those live in HVSC's side databases:
 *
 *   Songlengths.md5  INI file, `[Database]` then `md5=mm:ss mm:ss ...`
 *                    one entry per subsong, in order
 *   STIL.txt         `/<path>/Name.sid` followed by `TITLE:` / `ARTIST:` /
 *                    `COMMENT:` lines; subsong-specific parts are introduced
 *                    by `(#N)`
 *   SONGLENGTHS/     one .ssl per .sid: 2 bytes per subsong, BCD minutes and
 *                    seconds. What a C64-hosted player reads instead of the
 *                    10 MB md5 database.
 *
 * The md5 is over the whole file including the header, which is why it is
 * computed here rather than being read out of anything.
 */
#ifndef SIDMETA_H
#define SIDMETA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIDMETA_MD5_LEN 32

/* MD5 of the file's raw bytes, lowercase hex, NUL-terminated.
 * Returns 0 on success, -1 if the file cannot be read. */
int sidmeta_file_md5(const char *path, char out[SIDMETA_MD5_LEN + 1]);

/* ---- song length ---------------------------------------------------- *
 * All lookups take a 1-based subsong number, matching the PSID convention.
 * Times are milliseconds. A lookup that finds nothing returns -1 and leaves
 * *out_ms untouched; a lookup that finds the file but not that subsong
 * returns -2 (the databases are allowed to have gaps). */

/* Songlengths.md5 ("new" format: md5 over the whole file). */
int sidmeta_songlengths(const char *db_path, const char *md5,
                        unsigned subsong_1based, long *out_ms);

/* SONGLENGTHS/<same name>.ssl next to the .sid. BCD minutes and seconds. */
int sidmeta_ssl(const char *sid_path, unsigned subsong_1based, long *out_ms);

/* ---- STIL ----------------------------------------------------------- *
 * Fills title/artist for the given subsong. Both buffers may be NULL.
 * A file entry without a (#N) block applies to every subsong; a block
 * replaces the file-level text for the subsongs it covers.
 * Returns 0 if anything was found, -1 otherwise. */
int sidmeta_stil(const char *stil_path, const char *sid_path,
                 unsigned subsong_1based, char *title, size_t title_len,
                 char *artist, size_t artist_len);

/* ---- convenience ---------------------------------------------------- *
 * Try, in order: the .ssl next to the file, then Songlengths.md5. This is the
 * order a player wants - the local file is cheaper and is the one a user can
 * edit - and it keeps the caller from having to know both formats.
 * `db_path` may be NULL to skip the md5 database.
 * Returns the length in ms, or -1. */
long sidmeta_song_length_ms(const char *sid_path, unsigned subsong_1based,
                            const char *db_path);

#ifdef __cplusplus
}
#endif

#endif /* SIDMETA_H */