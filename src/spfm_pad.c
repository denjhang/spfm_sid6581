/*
 * spfm_pad.c - wire-time padding and $D418 decimation, shared by both players.
 *
 * Everything here used to live inside sidplay.c. It was moved out unchanged
 * when spfmplayer.exe needed the same behaviour: two copies of a timing engine
 * this fiddly would drift, and the one that drifts is always the one in use.
 *
 * See spfm_pad.h for why the filler exists. The short version: a digi writes
 * $D418 every ~136 us, which is 20.4 byte-times on the 1.5 Mbaud line, so
 * NOP bytes reproduce the spacing with UART jitter instead of host sleep
 * quantisation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spfm_pad.h"
#include "sid6581.h"
#include "spfm_transport.h"

/* 1.5 Mbaud 8N1: 10 bit per byte, so 1 us of wire is 0.15 bytes. */
#define WIRE_BYTES_PER_US  0.15

/* Lookback for the density gate: "how many $D418 writes in the last 250 ms". */
#define PAD_GATE_WIN_US    250000

spfm_pad_cfg_t spfm_pad;

static int     s_pad_active   = 0;   /* filler owns the pacing right now */
static double  s_pad_on_from  = -1;  /* music time of the first gated event */
static double  s_pad_on_to    = -1;  /* music time of the last gated event */
static int64_t s_wire_us      = 0;   /* wire time queued since the tune started */
static int64_t s_pad_credit   = 0;   /* us of filler budget in hand */
static int64_t s_pad_used_us  = 0;   /* total filler emitted */
static int     s_pad_skipped  = 0;   /* gaps refused for lack of budget */
static int     s_vol_seen     = 0;
static uint8_t s_vol_last     = 0;

/* Sliding window of (music_us, bytes) used by --pad-window. A global leaky
 * bucket averages over the whole tune, so a 20 ms burst can still ask for more
 * than the line carries and the host falls behind - which is heard as uneven
 * tempo. This cap is local: no window of --pad-window ms of music may exceed
 * --pad-local percent of the line, so padding is refused right where it would
 * overdraw instead of being averaged away. */
typedef struct { int64_t music_us; double bytes; } pad_win_t;
#define PAD_WIN_MAX 4096
static pad_win_t s_pad_win[PAD_WIN_MAX];
static int       s_pad_win_head, s_pad_win_n;
static double    s_pad_win_bytes;

void spfm_pad_defaults(void)
{
    memset(&spfm_pad, 0, sizeof spfm_pad);
    spfm_pad.pad_us   = -1;
    spfm_pad.duty     = 75;
    spfm_pad.lead_us  = 25000;
    spfm_pad.chunk    = 2048;
    spfm_pad.burst_ms = 200;
    spfm_pad.scope    = 0;
    spfm_pad.fit      = 1;
    spfm_pad.local    = 100;
    spfm_pad.gate     = 1;
    spfm_pad.rate     = 200;
    spfm_pad.hold_ms  = 60;
}

void spfm_pad_normalize(void)
{
    /* --pad cannot work without dropping repeated $D418 values: on Giana the
     * repeats are 66% of the volume writes, and padding every gap without that
     * reduction asks for more than 100% of the wire. Enabling it automatically
     * is the only setting that can produce sound, so there is no way to get it
     * wrong. */
    if (spfm_pad.pad_us > 0) spfm_pad.dedup_vol = 1;

    if (spfm_pad.duty     < 1)   spfm_pad.duty = 1;
    if (spfm_pad.duty     > 100) spfm_pad.duty = 100;
    if (spfm_pad.lead_us  < 1000) spfm_pad.lead_us = 1000;
    if (spfm_pad.chunk    < 256) spfm_pad.chunk = 256;
    if (spfm_pad.local    < 10)  spfm_pad.local = 10;
    if (spfm_pad.local    > 100) spfm_pad.local = 100;
    if (spfm_pad.rate     < 1)   spfm_pad.rate = 1;
    if (spfm_pad.hold_ms  < 0)   spfm_pad.hold_ms = 0;
    if (spfm_pad.burst_ms < 0)   spfm_pad.burst_ms = 0;
    if (spfm_pad.win_ms   < 0)   spfm_pad.win_ms = 0;
    if (spfm_pad.digi_hz  < 0)   spfm_pad.digi_hz = 0;
    if (spfm_pad.min_us   < 0)   spfm_pad.min_us = 0;
    if (spfm_pad.lag_us   < 0)   spfm_pad.lag_us = 0;
}

void spfm_pad_reset(void)
{
    s_pad_credit   = 0;
    s_pad_used_us  = 0;
    s_pad_skipped  = 0;
    s_wire_us      = 0;
    s_pad_win_head = s_pad_win_n = 0;
    s_pad_win_bytes = 0.0;
    s_pad_active   = 0;
    s_pad_on_from  = -1;
    s_pad_on_to    = -1;
    s_vol_seen     = 0;
    s_vol_last     = 0;
}

void spfm_pad_set_active(int on)
{
    s_pad_active = on ? 1 : 0;
}

int spfm_pad_active(void)
{
    return s_pad_active;
}

void spfm_pad_mark_on(double t_ms)
{
    if (s_pad_on_from < 0) s_pad_on_from = t_ms;
    s_pad_on_to = t_ms;
}

unsigned char *spfm_pad_gate_marks(const sid_event_t *ev, size_t n)
{
    if (spfm_pad.pad_us < 0 || !spfm_pad.gate || n == 0) return NULL;
    unsigned char *m = (unsigned char *)calloc(n, 1);
    if (!m) return NULL;

    /* One slot per volume write is enough: the window is a lookback over those
     * timestamps only, so no ring of all n events is needed. */
    int64_t *ts = (int64_t *)malloc(n * sizeof(*ts));
    if (!ts) { free(m); return NULL; }

    const int64_t hold_us = (int64_t)spfm_pad.hold_ms * 1000;
    size_t head = 0, cnt = 0;
    int64_t keep_until = -1;

    for (size_t i = 0; i < n; i++) {
        const int64_t t = (int64_t)(ev[i].t_ms * 1000.0);

        if (ev[i].reg == SID_REG_MODE_VOL) ts[cnt++] = t;

        while (head < cnt && ts[head] < t - PAD_GATE_WIN_US) head++;
        const size_t in_win = cnt - head;

        if ((int64_t)in_win * 1000000 / PAD_GATE_WIN_US >= spfm_pad.rate)
            keep_until = t + hold_us;

        m[i] = (keep_until >= 0 && t <= keep_until) ? 1 : 0;
    }

    free(ts);

    /* Nothing dense anywhere: return NULL instead of an all-zero array the
     * caller cannot tell apart from "gate broken". */
    for (size_t i = 0; i < n; i++)
        if (m[i]) return m;

    free(m);
    return NULL;
}

/* Streaming gate: the same decision, but the density history and the hold-off
 * survive between calls because a live .sid arrives one frame at a time.
 *
 * History is a fixed ring of the last PAD_GATE_WIN_US worth of $D418 stamps.
 * A frame is ~20 ms and the window is 250 ms, so the ring only has to hold a
 * few thousand entries even for a tune that writes $D418 every 136 us. */
#define PAD_GATE_RING 8192
static int64_t s_gate_ring[PAD_GATE_RING];
static size_t  s_gate_ring_n, s_gate_ring_head;
static int64_t s_gate_keep_until;

void spfm_pad_stream_begin(void)
{
    s_gate_ring_n = s_gate_ring_head = 0;
    s_gate_keep_until = -1;
}

unsigned char *spfm_pad_stream_marks(const sid_event_t *ev, size_t n)
{
    if (spfm_pad.pad_us < 0 || !spfm_pad.gate || n == 0) return NULL;

    unsigned char *m = (unsigned char *)calloc(n, 1);
    if (!m) return NULL;

    const int64_t hold_us = (int64_t)spfm_pad.hold_ms * 1000;
    int64_t keep_until = s_gate_keep_until;

    for (size_t i = 0; i < n; i++) {
        const int64_t t = (int64_t)(ev[i].t_ms * 1000.0);

        if (ev[i].reg == SID_REG_MODE_VOL) {
            /* Drop stamps that fell out of the window, then add this one. The
             * ring is ordered by time, so the oldest entry is the head. */
            while (s_gate_ring_n > 0 &&
                   s_gate_ring[s_gate_ring_head] < t - PAD_GATE_WIN_US) {
                s_gate_ring_head = (s_gate_ring_head + 1) % PAD_GATE_RING;
                s_gate_ring_n--;
            }
            if (s_gate_ring_n < PAD_GATE_RING) {
                const size_t tail = (s_gate_ring_head + s_gate_ring_n) % PAD_GATE_RING;
                s_gate_ring[tail] = t;
                s_gate_ring_n++;
            } else {
                /* Full: the window is smaller than the write rate can fill it,
                 * i.e. far denser than --pad-rate needs. 8192 entries in
                 * 250 ms is 32768 writes/s; treat it as saturated. */
                s_gate_ring[s_gate_ring_head] = t;
                s_gate_ring_head = (s_gate_ring_head + 1) % PAD_GATE_RING;
            }
        }

        if ((int64_t)s_gate_ring_n * 1000000 / PAD_GATE_WIN_US >= spfm_pad.rate)
            keep_until = t + hold_us;

        m[i] = (keep_until >= 0 && t <= keep_until) ? 1 : 0;
    }

    s_gate_keep_until = keep_until;

    for (size_t i = 0; i < n; i++)
        if (m[i]) return m;

    free(m);
    return NULL;
}

/* How many bytes the line can carry in `win_ms` of music. */
static double pad_window_cap(void)
{
    return 150000.0 * (double)spfm_pad.win_ms / 1000.0
                    * (double)spfm_pad.local / 100.0;
}

/* Drop entries older than win_ms behind `music_us`, then say whether `add`
 * bytes still fit. Returns 1 if they do. */
static int pad_window_fits(int64_t music_us, double add)
{
    if (spfm_pad.win_ms <= 0) return 1;

    const int64_t horizon = music_us - (int64_t)spfm_pad.win_ms * 1000;
    while (s_pad_win_n > 0 && s_pad_win[s_pad_win_head].music_us < horizon) {
        s_pad_win_bytes -= s_pad_win[s_pad_win_head].bytes;
        s_pad_win_head = (s_pad_win_head + 1) % PAD_WIN_MAX;
        s_pad_win_n--;
    }
    return (s_pad_win_bytes + add) <= pad_window_cap();
}

/* Record bytes emitted at a given musical position. */
static void pad_window_add(int64_t music_us, double bytes)
{
    if (spfm_pad.win_ms <= 0) return;
    if (s_pad_win_n == PAD_WIN_MAX) {
        /* Oldest entry is the only one we can drop without losing accuracy. */
        s_pad_win_bytes -= s_pad_win[s_pad_win_head].bytes;
        s_pad_win_head = (s_pad_win_head + 1) % PAD_WIN_MAX;
        s_pad_win_n--;
    }
    const int tail = (s_pad_win_head + s_pad_win_n) % PAD_WIN_MAX;
    s_pad_win[tail].music_us = music_us;
    s_pad_win[tail].bytes = bytes;
    s_pad_win_n++;
    s_pad_win_bytes += bytes;
}

int spfm_pad_vol_is_dup(const sid_event_t *e)
{
    if (!spfm_pad.dedup_vol || e->reg != SID_REG_MODE_VOL) return 0;
    const int dup = s_vol_seen && (uint8_t)e->val == s_vol_last;
    s_vol_seen = 1;
    s_vol_last = (uint8_t)e->val;
    return dup;
}

int64_t spfm_pad_gap(int64_t gap_us, int from_vol, int to_vol, int64_t music_us)
{
    if (!s_pad_active || gap_us <= 0) return 0;
    const int64_t cap_us = (int64_t)spfm_pad.burst_ms * 1000;

    /* Every command costs 26.67 us of wire; count it so the backlog figure
     * below reflects the real line, not just the filler. */
    s_wire_us += 27;

    /* The line is 1.5 Mbaud and the music is not. Filler emitted to reproduce a
     * gap still costs wire time, and once the queued wire has run ahead of the
     * music beyond --pad-lag the bytes cannot be played back on schedule any
     * more - FT_Write starts blocking, the host misses the next `due`, and the
     * whole tune slows down. That is the "tempo becomes uneven" report. Stop
     * padding until the music catches up: the sample stream keeps its internal
     * spacing, only some gaps fall back to host timing. */
    if (spfm_pad.lag_us > 0 &&
        s_wire_us - music_us > (int64_t)spfm_pad.lag_us) {
        s_pad_skipped++;
        return 0;
    }

    if (spfm_pad.scope == 1 && !from_vol && !to_vol) {
        /* Melody-to-melody: never padded. Its own spacing is what the host
         * deadlines are for, and padding it is what made whole tunes sound
         * too slow - the bytes were there but the music was still waiting on
         * the line. */
        s_pad_credit += gap_us * spfm_pad.duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    if (gap_us > spfm_pad.pad_us) {
        /* A long rest needs no filler, but it does refill the bucket. */
        s_pad_credit += gap_us * spfm_pad.duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    if (gap_us < spfm_pad.min_us) {
        s_pad_credit += gap_us * spfm_pad.duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    const int64_t cmd_us = 27;                    /* 4 bytes = 26.67 us */
    int64_t want = gap_us - cmd_us;
    if (want <= 0) return 0;

    s_pad_credit += gap_us * spfm_pad.duty / 100;
    if (s_pad_credit > cap_us) s_pad_credit = cap_us;

    int64_t spend = want < s_pad_credit ? want : s_pad_credit;

    /* Partial filler is the worst of both worlds: the gap ends up neither at its
     * recorded length nor at the host's natural pace, which is exactly what
     * makes a tune sound like it changes speed. So the budget is all-or-nothing
     * - either the whole gap is reproduced on the wire, or none of it is and the
     * host waits it out normally. */
    if (spfm_pad.fit && spend < want) { s_pad_skipped++; return 0; }

    if (spend <= 0) { s_pad_skipped++; return 0; }

    /* Local cap: refuse if the filler would push the last --pad-window ms of
     * music past --pad-local percent of the line. This is what stops the
     * uneven tempo - a global average cannot see a 20 ms burst asking for more
     * than the line has. */
    {
        const double add_bytes = (double)spend * WIRE_BYTES_PER_US;
        if (!pad_window_fits(music_us, add_bytes)) {
            s_pad_skipped++;
            return 0;
        }
        pad_window_add(music_us, add_bytes);
    }

    s_pad_credit -= spend;
    s_pad_used_us += spend;

    uint32_t nops = (uint32_t)((double)spend * WIRE_BYTES_PER_US + 0.5);
    if (nops) spfm_nop(nops);
    s_wire_us += (int64_t)((double)nops / WIRE_BYTES_PER_US + 0.5);
    return (int64_t)((double)nops / WIRE_BYTES_PER_US + 0.5);
}

/* ------------------------------------------------------------------ */
/* digi decimation                                                     */
/* ------------------------------------------------------------------ */

/* The digital drum is a stream of $D418 writes, one per raster interrupt, and
 * it is the only thing in a SID tune whose rate is anywhere near the link's
 * limit. Everything else (frequency, gate, ADSR, filter) is sparse and must be
 * left alone.
 *
 * --digi HZ puts those writes on a fixed 1/HZ grid: the first $D418 write in a
 * slot becomes that slot's sample, later writes in the same slot overwrite it
 * (sample and hold - the value the DAC would rest at by the end of the window),
 * and writes that repeat the already-latched value are dropped because the chip
 * is holding it. Nothing else in the stream is touched, and every timestamp
 * stays on the original time base, so the tune's length never changes.
 *
 * Measured on Giana s01: 6625 raw $D418/s include 66% repeats, so dedup alone
 * gets to ~2271/s; --digi 4000 keeps that whole rate. */
static spfm_digi_stats_t s_digi_stats;

const spfm_digi_stats_t *spfm_digi_stats(void)
{
    return &s_digi_stats;
}

sid_event_t *spfm_digi_decimate(const sid_event_t *ev, size_t n, size_t *out_n)
{
    *out_n = n;
    if (spfm_pad.digi_hz <= 0) return NULL;

    sid_event_t *out = (sid_event_t *)malloc(n * sizeof(*out));
    if (!out) return NULL;

    const double slot_ms = 1000.0 / (double)spfm_pad.digi_hz;
    size_t m = 0;
    long   cur_slot = -1;
    size_t slot_idx = 0;             /* index in `out` of this slot's sample */
    int    slot_has = 0;             /* 0 = this slot has no kept sample yet */
    int    have_held = 0;
    uint8_t held = 0;
    double  last_t = 0.0;

    s_digi_stats.in = s_digi_stats.out = 0;
    s_digi_stats.dropped_dup = s_digi_stats.merged = 0;

    /* Quantising to the slot start can land a sample before a melody write that
     * was emitted between the two, so every timestamp is clamped forward. */
    #define PUSH(x) do {                                  \
        sid_event_t _s = (x);                              \
        if (_s.t_ms < last_t) _s.t_ms = last_t;            \
        last_t = _s.t_ms;                                  \
        out[m++] = _s;                                     \
    } while (0)

    for (size_t i = 0; i < n; i++) {
        const sid_event_t *e = &ev[i];

        if (e->reg != SID_REG_MODE_VOL) {
            PUSH(*e);                  /* melody/gate/ADSR: never touched */
            continue;
        }

        s_digi_stats.in++;

        long slot = (long)(e->t_ms / slot_ms);
        if (slot < 0) slot = 0;

        if (slot == cur_slot) {
            if (slot_has) {
                /* Same slot: the last write wins (sample and hold). */
                out[slot_idx].val = e->val;
                s_digi_stats.merged++;
            } else if (have_held && e->val == held) {
                s_digi_stats.dropped_dup++;
            } else {
                /* First kept sample of a slot that previously only held dups. */
                sid_event_t s = *e;
                s.t_ms = (double)slot * slot_ms;
                PUSH(s);
                slot_idx = m - 1;
                slot_has = 1;
                held = e->val;
                have_held = 1;
                s_digi_stats.out++;
            }
            continue;
        }

        cur_slot = slot;
        slot_has = 0;

        if (have_held && e->val == held) {
            s_digi_stats.dropped_dup++;
            continue;
        }

        sid_event_t s = *e;
        s.t_ms = (double)slot * slot_ms;      /* quantise to the slot start */
        PUSH(s);
        slot_idx = m - 1;
        slot_has = 1;
        held = e->val;
        have_held = 1;
        s_digi_stats.out++;
    }
    #undef PUSH

    *out_n = m;
    return out;
}

/* ------------------------------------------------------------------ */
/* reporting                                                           */
/* ------------------------------------------------------------------ */

unsigned long spfm_pad_filler_us(void) { return (unsigned long)s_pad_used_us; }
int  spfm_pad_skipped(void)            { return s_pad_skipped; }

int spfm_pad_on_range(double *from_ms, double *to_ms)
{
    if (!spfm_pad.gate || s_pad_on_from < 0) return 0;
    if (from_ms) *from_ms = s_pad_on_from;
    if (to_ms)   *to_ms   = s_pad_on_to;
    return 1;
}

void spfm_pad_report(void)
{
    if (spfm_pad.pad_us < 0) return;

    /* A refused gap falls back to host timing. Some refusals are harmless
     * (long rests, tiny gaps); a steady stream of them is the cap biting
     * where the ear hears it as uneven tempo. */
    printf("\n  pad: %lld ms filler, %d gaps refused",
           (long long)(s_pad_used_us / 1000), s_pad_skipped);
    if (!spfm_pad.gate)
        printf(" (--pad-always)");
    else if (s_pad_on_from >= 0)
        printf(", gated on %.1f-%.1f s", s_pad_on_from / 1000.0,
               s_pad_on_to / 1000.0);
    else
        printf(" (no dense $D418 passage: nothing padded)");
}

void spfm_digi_report(double secs)
{
    if (spfm_pad.digi_hz <= 0) return;
    printf("\n  digi: %lu $D418 in -> %lu out", s_digi_stats.in, s_digi_stats.out);
    if (secs > 0.5) printf(" (%.0f/s)", s_digi_stats.out / secs);
    printf(", %lu merged, %lu duplicate-dropped",
           s_digi_stats.merged, s_digi_stats.dropped_dup);
}
