/*
 * spfm_pad.h - wire-time padding ("reverse flush-gap") and $D418 decimation.
 *
 * Shared by sidtest.exe (sidplay/sidlist) and spfmplayer.exe so the two
 * listening paths cannot drift apart. The mechanics live in spfm_pad.c; this
 * header is the configuration both programs fill in and the handful of calls
 * a replay loop has to make.
 *
 * Why padding exists: a C64 digi writes $D418 about every 136 us. Timing those
 * writes from the host means Windows' millisecond sleep quantisation, which is
 * seven times coarser than the gap, and the drum loses its timbre. 136 us is
 * exactly 20.4 byte-times at 1.5 Mbaud, so appending NOP bytes lets the UART
 * shift clock reproduce the spacing with 6.7 us jitter instead.
 */
#ifndef SPFM_PAD_H
#define SPFM_PAD_H

#include <stddef.h>
#include <stdint.h>

#include "sid_rsid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every knob is a field here rather than a file-static in each player, so a
 * CLI parser writes `spfm_pad.pad_us = ...` and the engine reads the same
 * value the other player does. Defaults are set by spfm_pad_defaults(). */
typedef struct {
    int pad_us;      /* pad gaps up to this many us; -1 = off */
    int duty;        /* percent of the wire the filler may use (75) */
    int lead_us;     /* how far the host may run ahead = latency (25000) */
    int chunk;       /* bytes queued before a forced flush (2048) */
    int burst_ms;    /* leaky-bucket ceiling, ms of wire (200) */
    int min_us;      /* never pad a gap shorter than this (0) */
    int scope;       /* 0 = every gap, 1 = only gaps touching $D418 */
    int fit;         /* 1 = all-or-nothing, 0 = allow half-filled gaps */
    int lag_us;      /* max wire-vs-music lag before padding pauses (0) */
    int win_ms;      /* local sliding window for the duty cap (0 = off) */
    int local;       /* max % of the line inside that window (100) */
    int digi_hz;     /* resample $D418 onto a 1/HZ grid (0 = off) */
    int dedup_vol;   /* drop $D418 writes that repeat the latched value */
    int gate;        /* 1 = pad only inside dense $D418 passages */
    int rate;        /* $D418 writes/s that count as a dense passage (200) */
    int hold_ms;     /* how far the drum credit outlasts the drum (60) */
} spfm_pad_cfg_t;

extern spfm_pad_cfg_t spfm_pad;

/* Fill the struct with the documented defaults. Call once before parsing. */
void spfm_pad_defaults(void);

/* Must run after option parsing: turns on the dedup that --pad cannot work
 * without, and clamps the values that have a valid range. */
void spfm_pad_normalize(void);

/* Clear the per-tune accounting. Call at the start of every replay. */
void spfm_pad_reset(void);

/* Which stretches of a finished stream get the filler. Returns a calloc'd
 * array of n flags (1 = padding allowed), or NULL when the gate is off
 * (--pad-always) or nothing in the stream is dense enough. The caller frees
 * it. */
unsigned char *spfm_pad_gate_marks(const sid_event_t *ev, size_t n);

/* Streaming variant, for a .sid that has no end and is therefore never handed
 * over as one array: libsidplayfp produces it one frame at a time, and frame N
 * is complete before frame N plays. Keeping the density history and the
 * hold-off across calls makes the gate marks continuous across frame
 * boundaries, so a live tune gets the same padding a recording does.
 *
 * The one gap a live stream cannot pad is the frame boundary itself: the next
 * frame does not exist yet. That gap is ~20 ms of silence, far longer than
 * --pad, so it would not be filled anyway. */
void          spfm_pad_stream_begin(void);
unsigned char *spfm_pad_stream_marks(const sid_event_t *ev, size_t n);

/* 1 while the filler owns the pacing. Set by the replay loop at every gated
 * boundary; also read by the transport flush rule, which must not try to hit
 * deadlines inside a padded passage. */
void spfm_pad_set_active(int on);
int  spfm_pad_active(void);

/* Record that this music timestamp was replayed with the filler on, so the
 * end-of-run line can report the range the gate actually used. */
void spfm_pad_mark_on(double t_ms);

/* Emit NOP bytes to fill a gap of `gap_us` between two register writes.
 * `from_vol`/`to_vol` say whether either side is a $D418 write, which
 * --pad-scope uses to restrict the filler to the drum. Returns how much of the
 * gap the filler covered, in us. */
int64_t spfm_pad_gap(int64_t gap_us, int from_vol, int to_vol,
                     int64_t music_us);

/* 1 if this $D418 write only repeats what the chip already latches. */
int spfm_pad_vol_is_dup(const sid_event_t *e);

/* Build a copy of the stream with $D418 resampled onto a 1/HZ grid. Returns
 * NULL when --digi is off, in which case the caller keeps the original. */
sid_event_t *spfm_digi_decimate(const sid_event_t *ev, size_t n, size_t *out_n);

/* Reporting for the end-of-run line. */
unsigned long spfm_pad_filler_us(void);
int           spfm_pad_skipped(void);
int           spfm_pad_on_range(double *from_ms, double *to_ms);   /* 0 = never */

typedef struct { unsigned long in, out, dropped_dup, merged; } spfm_digi_stats_t;
const spfm_digi_stats_t *spfm_digi_stats(void);

/* One-line summary of the pad accounting, or nothing when --pad is off. */
void spfm_pad_report(void);
void spfm_digi_report(double secs);

#ifdef __cplusplus
}
#endif

#endif /* SPFM_PAD_H */
