/*
 * sidplay.c - replay a SID register stream on the SPFM hardware.
 *
 * Two halves, matching the two things a .sid actually is:
 *
 *   1. Emulation (sid_rsid.c): run the 6502 init/play routines and record
 *      every $D400-$D41F write with a timestamp.
 *   2. Scheduling (this file): push those writes out over USB so each one
 *      reaches the SID at the same wall-clock offset it had in the original.
 *
 * The scheduling half is the interesting one. Naively sleeping "for N ms" per
 * event accumulates error, and a drift of 19.96 ms is one whole C64 frame, so
 * after a few minutes the music is visibly behind the melody it should have
 * been playing. Instead the player records one start timestamp and waits
 * against absolute deadlines (spfm_now_us + spfm_wait_until_us), so the error
 * stays bounded for the whole song.
 *
 * The pacing model follows Denjhang_Music_Player's VGM thread: work out how
 * far the clock says we should have progressed, emit everything due by now,
 * then yield. Being late is not fatal - the events still go out in order, just
 * bunched - so a slow USB transfer degrades timing rather than dropping notes.
 */
#include "sid_rsid.h"
#include "sidvgm.h"
#include "sidplay.h"
#include "sid6581.h"
#include "spfm_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#endif

#ifndef _WIN32
#define _mkdir(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

static sidplay_opts_t s_opt;

/* Registers defs worth suppressing while the melody is playing: leaving the
 * filter wide open is usually closer to what the composer heard on a 6581 than
 * whatever cutoff a 6581-specific init wrote. Off by default - see --no-touch. */
static int s_keep_filter = 0;

/* Flush granularity, ported from Denjhang_Music_Player v16 (AY8910 window
 * "Flush Mode"): 1 = register level (one USB transfer per register write),
 * 2 = command level (buffer every write, ship the batch at each wait). The
 * default is command level: a C64 play routine writes its whole frame in a
 * burst, so one transfer per frame lands in the silence before the next frame.
 * Register level is the tighter, more expensive setting for comparing timbre. */
static int s_flush_mode = 2;

/* Minimum wait (in sample ticks of the 735-sample PAL frame, reference units)
 * before the batch is shipped mid-frame. Ported as-is; 1 = flush at every
 * wait. It only matters for digi streams that interleave writes with short
 * waits. */
static int s_flush_threshold = 4;

/* Override for the above, in microseconds; -1 keeps the tick default.
 *
 * This is the knob that decides how many USB transfers a digi tune costs. A
 * C64 digi writes $D418 every ~136 us, which is longer than the 91 us default,
 * so nearly every sample triggers a transfer: Giana subsong 1 ends up at 6217
 * FT_Write/s averaging 4.6 bytes. FT232R cannot do that - one bulk transaction
 * per 1 ms USB frame puts the practical ceiling near 1000/s - so the driver
 * queues, USB frame-granular clumps come out, and the drum loses its
 * sub-millisecond structure.
 *
 * 0 is an alias for "one transfer per C64 frame" (20 ms). Anything else is the
 * shortest stretch of stream time worth one USB transfer. */
static int s_flush_gap_us = -1;

/* --flush-gap trades drum resolution for fewer transfers: the batch gets
 * coarser the larger the gap. Padding is the opposite trade, and it does not
 * need the host clock at all - see the block below ship_midframe for the
 * mechanics. s_pad_us is declared here because ship_midframe has to know that
 * padding is in charge of the pacing. */
static int     s_pad_us      = -1;   /* pad gaps up to this many us; -1 = off */

/* 1 while the filler owns the pacing for the gap being replayed.
 *
 * Padding replaces the host's per-event deadlines with "flush when the buffer
 * fills", which is only affordable where the stream is dense. On a PSID, or on
 * the intro of a digi tune, a frame writes a hundred-odd bytes, so a 2048-byte
 * chunk takes hundreds of milliseconds to fill: the music lags and the tempo
 * drifts. That is a property of the *pacing*, not of the filler, so --pad-scope
 * cannot fix it. Hence this flag: the gate below turns padding on only inside a
 * window where $D418 is written densely, and everywhere else the replay loop
 * uses the original deadline path untouched. */
static int     s_pad_active  = 0;
static int     s_pad_gate    = 1;    /* 0 = --pad-always: engage unconditionally */
static int     s_pad_rate    = 200;  /* $D418 writes/s needed to engage */
static int     s_pad_hold_ms = 60;   /* how far the drum credit outlasts the drum */
static double  s_pad_on_from = -1;   /* music time of the first gated event */
static double  s_pad_on_to   = -1;   /* music time of the last gated event */

/* True when the batch sitting in the buffer should go out now.
 *
 * With no --flush-gap this is the original rule: ship only when the next event
 * is far enough away that a transfer's cost is paid back. That is what every
 * PSID path has always done and it must not change.
 *
 * With --flush-gap MS it becomes "never let a byte sit for more than MS": the
 * batch also goes out once it has spanned that much of stream time. Dense digi
 * passages need that - their events are 136 us apart, so the old rule fires on
 * nearly every one of them and asks for 6000+ transfers/s, which is 6x what
 * FT232R can schedule (one bulk transfer per 1 ms USB frame). The bytes then
 * queue inside the driver and come out in 1-2 ms clumps, which is exactly the
 * "muffled, sounds resampled" report.
 *
 * `now_us` is passed in rather than read from the clock so the fast dry-run
 * path - which never sleeps - can drive the same decision from the stream's
 * own timestamps. `batch_first_us` is the due time of the oldest write still
 * buffered, or -1 when the buffer is empty. */
static int ship_midframe(int64_t now_us, int64_t next_due_us,
                         int64_t batch_first_us)
{
    /* With wire-time padding the byte stream carries the rhythm itself, so the
     * host must stop trying to hit deadlines: ship only when the transport
     * buffer fills (which it does by itself) or when the run ends. This applies
     * only inside a gated dense passage - outside it the original deadline rule
     * below must run unchanged or a sparse tune stalls. */
    if (s_pad_active) return 0;

    if (s_flush_gap_us < 0) {
        int64_t gap = (int64_t)s_flush_threshold * 1000000 / 44100;
        return now_us + gap <= next_due_us;
    }

    /* 0 is a convenience alias for "one transfer per C64 frame", which is the
     * coarsest setting that still keeps a tune in time. */
    const int64_t gap = s_flush_gap_us ? (int64_t)s_flush_gap_us : 20000;
    if (batch_first_us >= 0 && next_due_us - batch_first_us >= gap) return 1;
    return now_us + gap <= next_due_us;
}

/* ------------------------------------------------------------------ */
/* wire-time padding ("reverse flush-gap")                             */
/* ------------------------------------------------------------------ */

/* --flush-gap trades drum resolution for fewer transfers: the batch gets
 * coarser the larger the gap. This is the opposite trade, and it does not need
 * the host clock at all.
 *
 * A byte on the wire is 10 bits at 1.5 Mbaud = 6.6667 us, a fixed UART time
 * base that USB scheduling cannot disturb. A digi writes $D418 every ~136 us,
 * which is 20.4 byte-times; the Path A command is 4 of them, so appending ~16
 * NOP (0x80) bytes reproduces the spacing exactly. The NOPs carry no register
 * write - the firmware just burns them - so the SID sees nothing extra, and the
 * host no longer has to hit a deadline per sample: it only has to keep the
 * 64 KiB buffer non-empty.
 *
 * The cost is wire time, and it is not free. Padding every gap of Giana s01
 * would sit at 98-100% duty, which cannot work. Hence two rules:
 *
 *   - only gaps <= s_pad_us are padded (a long rest is the host's business);
 *   - a leaky bucket caps the filler at s_pad_duty percent of the wire, so a
 *     dense burst spends the silence around it instead of starving the stream.
 *
 * Dropping repeated $D418 values (--dedup-vol) is what makes the budget go far:
 * on Giana 66% of the volume writes repeat the value already latched, and the
 * chip is holding it, so removing them changes nothing audible. With dedup the
 * same 136 us spacing needs 27% of the wire instead of 92%. */
static int     s_pad_duty    = 75;   /* percent of the wire the filler may use */
static int     s_pad_lead_us = 25000;/* how far the host may run ahead (latency) */
static int     s_pad_chunk   = 2048; /* bytes queued before a forced flush */
static int     s_pad_burst_ms= 200;  /* leaky-bucket ceiling, ms of wire */
static int     s_pad_min_us  = 0;    /* never pad a gap shorter than this */
static int     s_pad_scope   = 0;    /* 0 = every gap, 1 = only gaps touching $D418 */
static int     s_pad_fit     = 1;    /* all-or-nothing: no half-filled gaps */
static int     s_pad_lag_us  = 0;    /* max wire-vs-music lag before padding pauses */
static int     s_pad_win_ms  = 0;    /* local sliding window for the duty cap */
static int     s_pad_local   = 100;  /* max % of the line inside that window */
static int64_t s_wire_us     = 0;    /* wire time queued since the tune started */

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
static int     s_dedup_vol   = 0;    /* drop $D418 writes that repeat the value */
static int64_t s_pad_credit  = 0;    /* us of filler budget in hand */
static int64_t s_pad_used_us = 0;    /* total filler emitted, for reporting */
static int     s_pad_skipped = 0;    /* gaps refused for lack of budget */
static int     s_vol_seen    = 0;
static uint8_t s_vol_last    = 0;

/* 1.5 Mbaud 8N1: 10 bit per byte, so 1 us of wire is 0.15 bytes. */
#define WIRE_BYTES_PER_US  0.15

static void pad_reset(void)
{
    s_pad_credit = 0;
    s_pad_used_us = 0;
    s_pad_skipped = 0;
    s_wire_us = 0;
    s_pad_win_head = s_pad_win_n = 0;
    s_pad_win_bytes = 0.0;
    s_pad_active = 0;
    s_pad_on_from = -1;
    s_pad_on_to   = -1;
    s_vol_seen   = 0;
    s_vol_last   = 0;
}

/* Which stretches of the stream are worth padding.
 *
 * The filler is only affordable, and only *useful*, where $D418 is written
 * densely. Everywhere else it costs the thing that made whole tunes stutter:
 * the host runs --pad-lead ahead of the music and leans on the transport buffer
 * to hide that lead, but a sparse passage produces far fewer bytes than the
 * wire drains, so the chip starves for the length of the lead and then gets a
 * clump. That is heard as a PSID, or the intro of a digi tune, "stuttering and
 * playing at the wrong tempo" - not as anything to do with the drum.
 *
 * So the marks are computed up front from the finished stream: an event is
 * marked when the $D418 write rate over the preceding PAD_GATE_WIN_US clears
 * --pad-rate, and for --pad-hold ms after the last time it did. The replay loop
 * then switches pacing exactly there, and uses the original host-deadline path
 * everywhere else - which is byte-for-byte what a plain PSID replay does.
 *
 * Returns NULL when the gate is off (--pad-always) or nothing is dense. */
#define PAD_GATE_WIN_US 250000
static unsigned char *pad_gate_marks(const sid_event_t *ev, size_t n)
{
    if (s_pad_us < 0 || !s_pad_gate || n == 0) return NULL;

    unsigned char *m = (unsigned char *)calloc(n, 1);
    if (!m) return NULL;

    /* One slot per volume write is enough: the window is a lookback over those
     * timestamps only, so no ring of all n events is needed. */
    int64_t *ts = (int64_t *)malloc(n * sizeof(*ts));
    if (!ts) { free(m); return NULL; }

    const int64_t hold_us = (int64_t)s_pad_hold_ms * 1000;
    size_t head = 0, cnt = 0;
    int64_t keep_until = -1;

    for (size_t i = 0; i < n; i++) {
        const int64_t t = (int64_t)(ev[i].t_ms * 1000.0);

        if (ev[i].reg == SID_REG_MODE_VOL) ts[cnt++] = t;

        while (head < cnt && ts[head] < t - PAD_GATE_WIN_US) head++;
        const size_t in_win = cnt - head;

        if ((int64_t)in_win * 1000000 / PAD_GATE_WIN_US >= s_pad_rate)
            keep_until = t + hold_us;

        m[i] = (keep_until >= 0 && t <= keep_until) ? 1 : 0;
    }

    free(ts);

    /* Nothing dense anywhere: say so instead of handing back an all-zero array
     * the caller cannot tell apart from "gate broken". */
    for (size_t i = 0; i < n; i++)
        if (m[i]) return m;

    free(m);
    return NULL;
}

/* How many bytes the line can carry in `win_ms` of music. */
static double pad_window_cap(void)
{
    return 150000.0 * (double)s_pad_win_ms / 1000.0 * (double)s_pad_local / 100.0;
}

/* Drop entries older than win_ms behind `music_us`, then say whether `add`
 * bytes still fit. Returns 1 if they do. */
static int pad_window_fits(int64_t music_us, double add)
{
    if (s_pad_win_ms <= 0) return 1;

    const int64_t horizon = music_us - (int64_t)s_pad_win_ms * 1000;
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
    if (s_pad_win_ms <= 0) return;
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

/* --pad cannot work without dropping repeated $D418 values: on Giana the
 * repeats are 66% of the volume writes, and padding every gap without that
 * reduction asks for more than 100% of the wire. Enabling it automatically is
 * the only setting that can produce sound, so there is no way to get it wrong. */
static void pad_normalize(void)
{
    if (s_pad_us > 0) s_dedup_vol = 1;
}

/* 1 if this $D418 write only repeats what the chip already latches. */
static int vol_is_dup(const sid_event_t *e)
{
    if (!s_dedup_vol || e->reg != SID_REG_MODE_VOL) return 0;
    const int dup = s_vol_seen && (uint8_t)e->val == s_vol_last;
    s_vol_seen = 1;
    s_vol_last = (uint8_t)e->val;
    return dup;
}

/* Emit NOP bytes to fill a gap of `gap_us` between two register writes.
 * `from_vol`/`to_vol` say whether either side of the gap is a $D418 write, which
 * --pad-scope uses to restrict the filler to the drum.
 * Returns how much of the gap the filler covered, in us, so the caller can wait
 * out the rest when the budget ran dry. */
static int64_t pad_gap2(int64_t gap_us, int from_vol, int to_vol, int64_t music_us)
{
    if (!s_pad_active || gap_us <= 0) return 0;
    const int64_t cap_us = (int64_t)s_pad_burst_ms * 1000;

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
    if (s_pad_lag_us > 0 && s_wire_us - music_us > (int64_t)s_pad_lag_us) {
        s_pad_skipped++;
        return 0;
    }

    if (s_pad_scope == 1 && !from_vol && !to_vol) {
        /* Melody-to-melody: never padded. Its own spacing is what the host
         * deadlines are for, and padding it is what made whole tunes sound
         * too slow - the bytes were there but the music was still waiting on
         * the line. */
        s_pad_credit += gap_us * s_pad_duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    if (gap_us > s_pad_us) {
        /* A long rest needs no filler, but it does refill the bucket. */
        s_pad_credit += gap_us * s_pad_duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    if (gap_us < s_pad_min_us) {
        s_pad_credit += gap_us * s_pad_duty / 100;
        if (s_pad_credit > cap_us) s_pad_credit = cap_us;
        return 0;
    }

    const int64_t cmd_us = 27;                    /* 4 bytes = 26.67 us */
    int64_t want = gap_us - cmd_us;
    if (want <= 0) return 0;

    s_pad_credit += gap_us * s_pad_duty / 100;
    if (s_pad_credit > cap_us) s_pad_credit = cap_us;

    int64_t spend = want < s_pad_credit ? want : s_pad_credit;

    /* Partial filler is the worst of both worlds: the gap ends up neither at its
     * recorded length nor at the host's natural pace, which is exactly what
     * makes a tune sound like it changes speed. So the budget is all-or-nothing
     * - either the whole gap is reproduced on the wire, or none of it is and the
     * host waits it out normally. */
    if (s_pad_fit && spend < want) { s_pad_skipped++; return 0; }

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
 * This is the knob the drum quality follows. It is independent of --pad:
 *
 *   --digi 8000   almost everything kept, hardest on the link
 *   --digi 4000   roughly one sample per 250 us
 *   --digi 2000   one per 500 us, clearly grittier
 *
 * Measured on Giana s01: 6625 raw $D418/s include 66% repeats, so dedup alone
 * gets to ~2271/s; --digi 4000 keeps that whole rate. */
static int s_digi_hz = 0;            /* 0 = leave $D418 alone */

typedef struct {
    unsigned long in, out, dropped_dup, merged;
} digi_stats_t;

static digi_stats_t s_digi_stats;

/* Build a filtered copy of the stream. Returns NULL when nothing is enabled,
 * in which case the caller keeps using the original array. */
static sid_event_t *digi_decimate(const sid_event_t *ev, size_t n, size_t *out_n)
{
    if (s_digi_hz <= 0) return NULL;

    sid_event_t *out = (sid_event_t *)malloc(n * sizeof(*out));
    if (!out) return NULL;

    const double slot_ms = 1000.0 / (double)s_digi_hz;
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
/* hardware                                                            */
/* ------------------------------------------------------------------ */

static int hw_connect(void)
{
    if (s_opt.dry_run) {
        spfm_dry_run_enable(true);
        return 0;
    }

    uint32_t ids[16];
    char descs[16][64];
    int n = spfm_list_devices(ids, 16, descs);
    if (n <= 0) {
        fprintf(stderr, "no FTDI device found (is the SPFM plugged in?)\n");
        return -1;
    }
    if (s_opt.device >= n) {
        fprintf(stderr, "--device %d out of range, %d present\n", s_opt.device, n);
        return -1;
    }
    if (spfm_open(s_opt.device) != 0) return -1;
    printf("device %d: %s\n", s_opt.device, descs[s_opt.device]);
    return 0;
}

static void hw_disconnect(void)
{
    if (!s_opt.dry_run) spfm_close();
}

/* Stop all three voices without waiting for the release phase, so a song
 * change does not carry the previous tune's tail into the next one. */
static void hw_mute(void)
{
    /* A padded stream can have tens of ms of music queued. Send the mute now
     * instead of after it, otherwise the previous tune keeps playing over the
     * pause and the next track starts late. */
    spfm_purge_tx();
    for (uint8_t v = 0; v < 3; v++)
        sid_write(s_opt.slot, (uint8_t)(v * 7 + SID_REG_CONTROL), 0x00);
    sid_write(s_opt.slot, SID_REG_MODE_VOL, 0x00);
    sid_flush();
}

/* ------------------------------------------------------------------ */
/* keyboard (playlist control)                                         */
/* ------------------------------------------------------------------ */

/* A dedicated blocking read thread, not a poll.
 *
 * The first version called _kbhit() between events, which means a key only
 * registered while the player happened to be waiting for the next note, and
 * never at all while a track was being flushed or the pause between tracks
 * ran. Keys are supposed to interrupt immediately, so the reader blocks in
 * _getch() and just records the latest request; the replay loop checks that
 * record at every event and drops whatever it is doing. */

static volatile int s_action = 0;      /* 'q','n','b',',','.' or 0 */

static int kb_read_blocking(void)
{
#ifdef _WIN32
    /* _getch() talks to the console directly (CONIN$), so it ignores redirected
     * stdin and returns EOF the moment there is no console. Read fd 0 instead
     * when stdin is not a terminal, which is what makes piped input - and the
     * SIDPLAY_KEYS test path - actually reach the reader. */
    if (!_isatty(_fileno(stdin))) {
        unsigned char c;
        if (_read(0, &c, 1) != 1) return -1;
        return c;
    }
    int c = _getch();
    /* Arrow and function keys arrive as two bytes: 0x00 or 0xE0 then a code.
     * Swallow the pair so it cannot be mistaken for ',' or '.'. */
    if (c == 0 || c == 0xE0) { _getch(); return 0; }
    return c;
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
#endif
}

static void kb_thread_body(void)
{
    for (;;) {
        int c = kb_read_blocking();
        if (c < 0) break;              /* stdin closed, nothing more to read */
        switch (c) {
        case 'q': case 'Q': case 27: s_action = 'q'; break;
        case 'n': case 'N':          s_action = 'n'; break;
        case 'b': case 'B':          s_action = 'b'; break;
        case ',':                    s_action = ','; break;
        case '.':                    s_action = '.'; break;
        default: break;
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall kb_thread(void *arg)
{
    (void)arg;
    kb_thread_body();
    return 0;
}
#else
static void *kb_thread(void *arg)
{
    (void)arg;
    kb_thread_body();
    return NULL;
}
#endif

static void kb_start(void)
{
#ifdef _WIN32
    /* No console at all (output redirected into a file, run from a scheduler):
     * _getch() would fail immediately and the reader would spin. SIDPLAY_KEYS=1
     * forces the reader on anyway, which is how the interrupt path is tested
     * with piped input. */
    if (!_isatty(_fileno(stdin)) && !getenv("SIDPLAY_KEYS")) return;
    uintptr_t h = _beginthreadex(NULL, 0, kb_thread, NULL, 0, NULL);
    if (h) CloseHandle((HANDLE)h);
#else
    static int raw_done = 0;
    if (!raw_done && isatty(STDIN_FILENO)) {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_lflag &= ~(ICANON | ECHO);
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        raw_done = 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, kb_thread, NULL);
    pthread_detach(th);
#endif
}

/* Take the pending request, if any. Consuming it is what lets the caller tell
 * "the user pressed something this track" from "nothing happened". */
static int kb_take(void)
{
    int a = s_action;
    if (a) s_action = 0;
    return a;
}

/* The key that interrupted the last replay. replay() consumes the request to
 * decide it should stop, but the playlist still needs to know *which* key it
 * was to pick the next track, so the choice is kept here. */
static int s_interrupt = 0;

/* ------------------------------------------------------------------ */
/* event stream loading                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    sid_event_t *events;     /* owned unless `borrowed` */
    size_t       count;
    int          borrowed;   /* sid_sidreg_read hands back a static buffer */
    sid_song_info_t info;
} stream_t;

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    s += a - b;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

/* Whether the file needs the 6502 emulator, or is already a rendered stream. */
static int is_sidreg(const char *path)
{
    return has_suffix(path, ".sidreg");
}

/* A rendered stream in the project's own container. Checked by magic as well
 * as by name, so a renamed file still plays instead of being fed to the 6502. */
static int is_sidvgm(const char *path)
{
    return has_suffix(path, ".sidvgm") || sidvgm_is_file(path);
}

/* True for both pre-rendered containers: they are replayed, never emulated. */
static int is_rendered(const char *path)
{
    return is_sidreg(path) || is_sidvgm(path);
}

static const char *g_cache_dir_override = NULL;

void sidplay_set_cache_dir(const char *dir)
{
    g_cache_dir_override = dir;
}

static int cache_dir_for(const char *sid_path, char *buf, size_t len)
{
    (void)sid_path;
    if (g_cache_dir_override && *g_cache_dir_override) {
        snprintf(buf, len, "%s", g_cache_dir_override);
        return 0;
    }
    /* Default: sid_cache in the working directory, matching the STC32G tools
     * layout so the two projects can share rendered streams. */
    snprintf(buf, len, "sid_cache");
    return 0;
}

static void stream_free(stream_t *st)
{
    if (!st->borrowed && st->events) sid_song_free(st->events);
    st->events = NULL;
    st->count = 0;
}

static int load_stream(const char *path, int subsong, double seconds,
                       int use_cache, stream_t *st, char *err, size_t errlen)
{
    memset(st, 0, sizeof(*st));

    /* A pre-rendered stream needs no emulation at all - useful when iterating
     * on playback timing without paying the 6502 run every time. */
    if (is_sidvgm(path)) {
        /* sidvgm_read hands back a malloc'd buffer, so it is owned here. The
         * header carries the clock/region and the GD3 tags, so unlike .sidreg
         * the info block is real rather than invented. */
        if (sidvgm_read(path, &st->events, &st->count, &st->info) != 0) {
            snprintf(err, errlen, "cannot read %s as sidvgm", path);
            return -1;
        }
        if (!st->info.name[0]) {
            const char *base = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') base = p + 1;
            snprintf(st->info.name, sizeof st->info.name, "%s", base);
        }
        /* A recording is longer than an audition needs to be. Trimming here
         * (rather than in replay) means --seconds shortens a .sidvgm too. */
        if (seconds > 0.0 && seconds < 1e8) {
            const double limit = seconds * 1000.0;
            while (st->count > 0 && st->events[st->count - 1].t_ms > limit)
                st->count--;
        }
        return 0;
    }

    if (is_sidreg(path)) {
        if (sid_sidreg_read(path, &st->events, &st->count) != 0) {
            snprintf(err, errlen, "cannot read %s", path);
            return -1;
        }
        st->borrowed = 1;
        /* info.name is shown as the song title, so give it the file's stem
         * rather than the full path (which would also get cut at 32 chars). */
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        snprintf(st->info.name, sizeof(st->info.name), "%s", base);
        st->info.pal = 1;
        st->info.vsync_hz = 50.125;
        st->info.clock_hz = 985248.0;
        st->info.n_songs = 1;
        return 0;
    }

    if (use_cache) {
        char cache_dir[512], cache_path[768];
        cache_dir_for(path, cache_dir, sizeof cache_dir);
        if (sid_sidreg_cache_name(cache_path, sizeof cache_path, cache_dir,
                                  path, subsong, seconds) == 0 &&
            sid_sidreg_exists(cache_path)) {
            if (sid_sidreg_read(cache_path, &st->events, &st->count) == 0) {
                st->borrowed = 1;
                sid_song_probe(path, &st->info);
                printf("  cache: %s (%lu events)\n", cache_path,
                       (unsigned long)st->count);
                return 0;
            }
        }
    }

    if (sid_song_extract(path, subsong, seconds, &st->events, &st->count,
                         &st->info, err, errlen) != 0) {
        return -1;
    }

    if (use_cache && st->count) {
        char cache_dir[512], cache_path[768];
        cache_dir_for(path, cache_dir, sizeof cache_dir);
        if (sid_sidreg_cache_name(cache_path, sizeof cache_path, cache_dir,
                                  path, subsong, seconds) == 0) {
            /* Failing to write the cache is never worth aborting playback. */
            _mkdir(cache_dir);
            sid_sidreg_write(cache_path, st->events, st->count);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the replay loop                                                     */
/* ------------------------------------------------------------------ */

static void report_link_stats(size_t nevents, double secs);

/* Ship everything buffered. In register mode deferred flush is off, so each
 * sid_write() has already pushed its own transfer; this is the frame-boundary
 * flush for command mode and a no-op-by-then flush for register mode. */
static void ship(void)
{
    sid_flush();
}

/* Emit one event. Every register is written straight through: the stream is
 * already in the order the original code produced it, and any reordering would
 * break the gate/reset sequences that produce percussion and digi samples. */
static void emit(const sid_event_t *e)
{
    if (s_keep_filter) {
        /* Optional: drop the song's filter programming so the raw voices stay
         * bright. Register 0x18 is a special case - it carries the master
         * volume in its low nibble, so instead of skipping it we keep the
         * volume and clear the filter mode. Skipping it outright would mute
         * the chip. */
        if (e->reg == SID_REG_RES_FILT ||
            e->reg == SID_REG_FC_LO ||
            e->reg == SID_REG_FC_HI)
            return;
        if (e->reg == SID_REG_MODE_VOL) {
            sid_write(s_opt.slot, e->reg, (uint8_t)(e->val & SID_VOLUME_MASK));
            if (s_flush_mode == 1) sid_flush();
            return;
        }
    }
    sid_write(s_opt.slot, e->reg, e->val);
    if (s_flush_mode == 1) sid_flush();
}

/* Play a .sid forever, running the 6502 live.
 *
 * A .sid has no end: the play routine is meant to be called indefinitely and
 * loops inside itself. Rendering a fixed buffer therefore imposes an arbitrary
 * duration, which is what the earlier version did (and why tracks stopped at
 * 60 seconds). Here the emulator is advanced one frame at a time, and each
 * frame's writes are emitted at the times they carry.
 *
 * Cost: one play() call is tens of microseconds against the ~20 ms the
 * previous frame is playing, so the emulation hides inside the playback and
 * there is no audible stall. The frame is emitted as soon as it is ready, and
 * the wait for the next frame is the only place the process blocks.
 *
 * Returns 0 if the user asked for the next track, -1 if they asked to quit. */
static int stream_play_sid(const char *path, int subsong, double speed,
                           sid_song_info_t *out_info)
{
    char err[128];
    sid_song_info_t info;
    if (sid_stream_open(path, subsong, &info, err, sizeof err) != 0) {
        fprintf(stderr, "  %s\n", err);
        return -1;
    }
    if (out_info) *out_info = info;

    const double frame_ms = sid_stream_frame_ms();
    const double us_per_ms = 1000.0 / speed;
    const int64_t start = spfm_now_us();

    /* The init writes are the first frame; they have t_ms == 0 and must all
     * land before the first play frame, so they are emitted without waiting. */
    const sid_event_t *ev = NULL;
    size_t n = 0;
    if (sid_stream_next_frame(&ev, &n) != 0) {
        sid_stream_close();
        return -1;
    }
    sid_set_deferred_flush(s_flush_mode == 2);
    spfm_flush_stats_reset();
    /* A live .sid is paced by its own deadlines, so the default 64 KiB batch is
     * right; make sure a padded track earlier in the playlist did not leave the
     * small flush size behind. */
    spfm_set_flush_bytes(SPFM_BUF_SIZE);
    for (size_t i = 0; i < n; i++) emit(&ev[i]);
    ship();

    int64_t worst_lag_us = 0;
    double played_ms = 0.0;
    int64_t next_report = 1000;      /* ms of music between progress updates */

    for (;;) {
        if (sid_stream_next_frame(&ev, &n) != 0) break;

        /* Emit this frame's writes, waiting for each one's due time. The
         * frame's timestamps are absolute from the song start, so they stay
         * correct no matter how many frames have gone by. */
        int64_t batch_first = -1;
        for (size_t i = 0; i < n; i++) {
            int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);
            if (due > spfm_now_us()) {
                /* Command mode: only ship the batch when the upcoming wait is
                 * long enough to pay for a USB transfer, so a burst of writes
                 * stays in one transfer; the frame boundary below always
                 * flushes. With --flush-gap the batch also goes out once it has
                 * spanned that much stream time, which is what keeps a digi
                 * tune under the USB transfer ceiling. */
                if (ship_midframe(spfm_now_us(), due, batch_first)) {
                    ship();
                    batch_first = -1;
                }
                while (due > spfm_now_us()) {
                    int k = kb_take();
                    if (k) {
                        s_interrupt = k;
                        sid_set_deferred_flush(false);
                        sid_flush();
                        sid_stream_close();
                        return k == 'q' ? -1 : 0;
                    }
                    int64_t left = due - spfm_now_us();
                    spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
                }
            } else {
                int64_t lag = spfm_now_us() - due;
                if (lag > worst_lag_us) worst_lag_us = lag;
            }
            if (batch_first < 0) batch_first = due;
            emit(&ev[i]);
        }
        sid_flush();          /* frame boundary: always ship */

        /* Interrupt check between frames as well: a sparse song may have no
         * event to wait on for hundreds of milliseconds. */
        int k = kb_take();
        if (k) {
            s_interrupt = k;
            sid_set_deferred_flush(false);
            sid_flush();
            sid_stream_close();
            return k == 'q' ? -1 : 0;
        }

        played_ms += frame_ms;

        if (played_ms >= next_report) {
            next_report = (int64_t)(played_ms / 5000 + 1) * 5000;
            printf("\r  %4.0f s  lag %ld ms    ",
                   played_ms / 1000.0, (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    sid_stream_close();
    report_link_stats(0, played_ms / 1000.0);
    return 0;
}

/* Replay a pre-rendered `.sidreg` buffer. Returns 0 at the end of the buffer,
 * -1 if the user interrupted. */

/* A stream can be byte-exact and still break: FT232R schedules one bulk
 * transaction per 1 ms USB frame, so what has to fit is the number of
 * transfers, not their total size. Printed by both replay paths so a
 * --flush-gap change can be judged without guessing. */
static void report_link_stats(size_t nevents, double secs)
{
    unsigned long wr = 0, bytes = 0;
    size_t maxw = 0;
    spfm_flush_stats(&wr, &bytes, &maxw);
    printf("  link: %lu transfers", wr);
    if (secs > 0.5) printf(" (%.0f/s)", wr / secs);
    printf(", %lu bytes", bytes);
    if (wr) printf(", avg %.1f B, largest %lu B",
                   (double)bytes / (double)wr, (unsigned long)maxw);
    /* 1.5 Mbaud 8N1: 150000 bytes/s of wire. Above 85% the filler is spending
     * the rests that the next burst needs, which is the first sign that --pad
     * is set too high for this tune. */
    if (secs > 0.5) {
        const double duty = 100.0 * (double)bytes / (secs * 150000.0);
        printf(", wire %.0f%%", duty);
        if (duty > 85.0) printf("  (over budget: lower --pad or --pad-duty)");
    }
    if (s_digi_hz > 0) {
        printf("\n  digi: %lu $D418 in -> %lu out", s_digi_stats.in,
               s_digi_stats.out);
        if (secs > 0.5) printf(" (%.0f/s)", s_digi_stats.out / secs);
        printf(", %lu merged, %lu duplicate-dropped",
               s_digi_stats.merged, s_digi_stats.dropped_dup);
    }
    if (s_pad_us >= 0) {
        /* A refused gap falls back to host timing. Some refusals are harmless
         * (long rests, tiny gaps); a steady stream of them is the cap biting
         * where the ear hears it as uneven tempo. */
        printf("\n  pad: %lld ms filler, %d gaps refused",
               (long long)(s_pad_used_us / 1000), s_pad_skipped);
        if (!s_pad_gate)
            printf(" (--pad-always)");
        else if (s_pad_on_from >= 0)
            printf(", gated on %.1f-%.1f s", s_pad_on_from / 1000.0,
                   s_pad_on_to / 1000.0);
        else
            printf(" (no dense $D418 passage: nothing padded)");
    }
    printf("\n");
    (void)nevents;
}

static int replay(const sid_event_t *ev_in, size_t n_in, double speed, int pace)
{
    if (n_in == 0) return 0;
    pad_reset();

    /* --digi works on the finished stream, before anything is sent: it decides
     * which $D418 writes exist at all, so it is upstream of --pad and of the
     * flush rules. Everything else in the stream passes through untouched. */
    size_t n = n_in;
    sid_event_t *decimated = digi_decimate(ev_in, n_in, &n);
    const sid_event_t *ev = decimated ? decimated : ev_in;
    if (n == 0) { free(decimated); return 0; }

    /* Padded mode produces bytes much faster than 1.5 Mbaud drains them, so the
     * transport must not wait for its 64 KiB buffer to fill: 64 KiB is 437 ms
     * of wire queued behind every flush, which is both the track-change latency
     * and what jammed the link. --pad-chunk sets the real size. The unpaced dry
     * run needs the same setting to report the board's transfer pattern. */
    const int pad_cfg  = (s_pad_us >= 0);
    const int gate_cfg = pad_cfg && s_pad_gate;   /* marks decide, not the flag */
    spfm_set_flush_bytes(pad_cfg ? (size_t)s_pad_chunk : SPFM_BUF_SIZE);

    /* Which stretches actually get the filler. In gate mode a NULL result
     * means "no dense passage in this tune", i.e. pad nowhere; with
     * --pad-always the flag itself decides. */
    unsigned char *gate = pad_gate_marks(ev, n);
    const int pad_all = pad_cfg && !gate_cfg;     /* --pad-always */

    /* Dry run: walk the stream as fast as possible unless --pace was given.
     * Pacing would only prove that Sleep() works, and waiting 60 s to check
     * that the bytes came out right is not a useful test. */
    if (s_opt.dry_run && !pace) {
        spfm_flush_stats_reset();
        sid_set_deferred_flush(s_flush_mode == 2);
        int64_t batch_first = -1;
        for (size_t i = 0; i < n; i++) {
            /* Drive the real mid-frame decision from the stream's own clock so
             * this path reports the transfer pattern the board would see. */
            const int64_t now = (int64_t)(ev[i].t_ms * 1000.0);
            const int64_t next = (i + 1 < n) ? (int64_t)(ev[i + 1].t_ms * 1000.0)
                                             : now;
            const int pad_on = gate ? gate[i] : pad_all;
            if (pad_on != s_pad_active) {
                sid_flush();          /* same boundary flush as the live path */
                s_pad_active = pad_on;
                spfm_set_flush_bytes(pad_on ? (size_t)s_pad_chunk : SPFM_BUF_SIZE);
            }
            if (s_pad_active) {
                if (s_pad_on_from < 0) s_pad_on_from = ev[i].t_ms;
                s_pad_on_to = ev[i].t_ms;
            }
            if (ship_midframe(now, next, batch_first)) {
                sid_flush();
                batch_first = -1;
            }
            if (batch_first < 0) batch_first = now;
            if (!vol_is_dup(&ev[i])) emit(&ev[i]);
            if (i + 1 < n)
                pad_gap2(next - now, ev[i].reg == SID_REG_MODE_VOL,
                         ev[i + 1].reg == SID_REG_MODE_VOL, next);
        }
        sid_flush();
        sid_set_deferred_flush(false);
        printf("  dry-run: %lu events -> %lu bytes\n",
               (unsigned long)n, (unsigned long)spfm_capture_size());
        report_link_stats(n, ev[n - 1].t_ms / 1000.0);
        free(gate);
        free(decimated);
        return 0;
    }

    const double us_per_ms = 1000.0 / speed;
    int64_t start = spfm_now_us();
    int64_t worst_lag_us = 0;
    size_t last_report = (size_t)-1;

    /* One USB transfer per frame instead of one per register write. A C64 play
     * routine writes its whole frame's worth of registers in a burst, so the
     * events arrive in tight groups separated by ~20 ms of silence; the flush
     * below lands in that silence and the bytes still reach the SID before the
     * next frame is due. Doing it per write instead would put 50 latency-timer
     * expiries inside one frame and the music would fall apart. */
    sid_set_deferred_flush(s_flush_mode == 2);
    spfm_flush_stats_reset();
    pad_reset();

    /* Padded mode keeps the host's schedule, but as a soft bound instead of a
     * per-event deadline: the host may run this far ahead of the music, never
     * more. Inside that window the NOP filler gives every short gap exact UART
     * timing (no host jitter). The bound IS the playback latency, so it is
     * exposed as --pad-lead. */
    #define PAD_LEAD_US (s_pad_lead_us)

    if (gate || pad_all) {
        spfm_purge_tx();
    } else if (pad_cfg) {
        /* --pad was asked for but no passage is dense enough to use it: this
         * tune replays exactly like a plain one. */
        printf("  pad: no dense $D418 passage (needs ~%d writes/s); "
               "replaying without filler\n", s_pad_rate);
        spfm_set_flush_bytes(SPFM_BUF_SIZE);
    } else {
        spfm_set_flush_bytes(SPFM_BUF_SIZE);
    }

    int64_t batch_first = -1;
    for (size_t i = 0; i < n; i++) {
        int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);

        /* The pacing mode is a property of this event, not of the run: inside a
         * drum passage the filler owns the timing, outside it the original
         * deadline rule does. Switch both the pacing flag and the transport
         * chunk size at the boundary, so a sparse section batches exactly like
         * a plain PSID replay. */
        {
            const int pad_on = gate ? gate[i] : pad_all;
            if (pad_on != s_pad_active) {
                /* Both directions need a push. Leaving a padded passage, the
                 * filler has run up to --pad-lead ahead and those bytes are
                 * still queued; entering one, the sparse path may be holding
                 * up to 64 KiB. Either way the chunk size is about to change,
                 * so drain first - the wait is affordable on both sides. */
                sid_flush();
                s_pad_active = pad_on;
                spfm_set_flush_bytes(pad_on ? (size_t)s_pad_chunk : SPFM_BUF_SIZE);
            }
            if (pad_on) {
                if (s_pad_on_from < 0) s_pad_on_from = ev[i].t_ms;
                s_pad_on_to = ev[i].t_ms;
            }
        }

        if (s_pad_active) {
            const int64_t target = due - PAD_LEAD_US;
            while (spfm_now_us() < target) {
                int k = kb_take();
                if (k) {
                    s_interrupt = k;
                    sid_set_deferred_flush(false);
                    sid_flush();
                    free(gate);
                    return -1;
                }
                int64_t left = target - spfm_now_us();
                spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
            }
        } else if (due > spfm_now_us()) {
            if (ship_midframe(spfm_now_us(), due, batch_first)) {
                ship();       /* ship what is due so far, then block */
                batch_first = -1;
            }
            /* Interruptible wait: never sleep longer than a few ms, so a key
             * press takes effect straight away instead of at the next note. */
            while (due > spfm_now_us()) {
                int k = kb_take();
                if (k) {
                    s_interrupt = k;
                    sid_set_deferred_flush(false);
                    sid_flush();
                    free(gate);
                    return -1;
                }
                int64_t left = due - spfm_now_us();
                spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
            }
        } else {
            int64_t lag = spfm_now_us() - due;
            if (lag > worst_lag_us) worst_lag_us = lag;
        }

        if (batch_first < 0) batch_first = due;
        if (!vol_is_dup(&ev[i])) emit(&ev[i]);
        /* Filler goes AFTER this write and before the next one, so the wire
         * order is [cmd][nop...][cmd] and the gap is reproduced exactly. */
        if (i + 1 < n)
            pad_gap2((int64_t)((ev[i + 1].t_ms - ev[i].t_ms) * 1000.0),
                     ev[i].reg == SID_REG_MODE_VOL,
                     ev[i + 1].reg == SID_REG_MODE_VOL,
                     (int64_t)(ev[i + 1].t_ms * 1000.0));

        /* Dense passages have no gap long enough to land in, so check here as
         * well: one compare per event is nothing next to the USB write. */
        int k = kb_take();
        if (k) {
            s_interrupt = k;
            sid_set_deferred_flush(false);
            sid_flush();
            free(gate);
            return -1;
        }

        /* Progress is reported per second of music, not per event: at 1200
         * events/s a per-event line would be unreadable. */
        size_t sec = (size_t)(ev[i].t_ms / 1000.0);
        if (sec != last_report) {
            last_report = sec;
            printf("\r  %5.1f s  event %lu/%lu  lag %ld ms   ",
                   ev[i].t_ms / 1000.0,
                   (unsigned long)(i + 1), (unsigned long)n,
                   (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    printf("\r  %5.1f s  %lu events  worst lag %ld ms        \n",
           ev[n - 1].t_ms / 1000.0, (unsigned long)n,
           (long)(worst_lag_us / 1000));
    report_link_stats(n, ev[n - 1].t_ms / 1000.0);
    free(gate);
    free(decimated);
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void print_info(const sid_song_info_t *in, const char *path)
{
    printf("file      : %s\n", path);
    printf("title     : %s\n", in->name[0] ? in->name : "(none)");
    printf("author    : %s\n", in->author[0] ? in->author : "(none)");
    printf("format    : %s\n", in->is_rsid ? "RSID" : "PSID");
    printf("clock     : %s (%.3f Hz cpu, %.3f Hz vsync)\n",
           in->pal ? "PAL" : "NTSC", in->clock_hz, in->vsync_hz);
    printf("subsongs  : %u (default %u)\n", in->n_songs, in->start_song);
    printf("load=0x%04X init=0x%04X play=0x%04X\n",
           in->load_addr, in->init_addr, in->play_addr);
    printf("speed     : 0x%08lX%s\n", (unsigned long)in->speed,
           in->speed == 0 ? " (vblank)" : " (CIA timer)");
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: sidinfo <file.sid>\n");
        return 1;
    }
    sid_song_info_t in;
    if (sid_song_probe(argv[0], &in) != 0) {
        fprintf(stderr, "cannot read header of %s\n", argv[0]);
        return 1;
    }
    print_info(&in, argv[0]);
    return 0;
}

static int cmd_extract(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: sidextract <file.sid> <out.sidreg> [--seconds N] "
                "[--subsong N]\n");
        return 1;
    }
    const char *in_path = argv[0];
    const char *out_path = argv[1];
    double seconds = 60.0;
    int subsong = -1;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option '%s'\n", argv[i]); return 1; }
    }

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t in;
    char err[128];

    printf("running 6502 for %.1f s of music...\n", seconds);
    if (sid_song_extract(in_path, subsong, seconds, &ev, &n, &in, err, sizeof err) != 0) {
        fprintf(stderr, "extract failed: %s\n", err);
        return 1;
    }
    printf("%s: %lu register writes\n", in.name, (unsigned long)n);

    if (sid_sidreg_write(out_path, ev, n) != 0) {
        fprintf(stderr, "cannot write %s\n", out_path);
        sid_song_free(ev);
        return 1;
    }
    printf("-> %s\n", out_path);
    sid_song_free(ev);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    const char *path = NULL;
    double seconds = 0.0;          /* 0 = forever, the default */
    double speed = 1.0;
    int subsong = -1;
    int use_cache = 1;
    int pace = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cache"))                use_cache = 0;
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (!strcmp(argv[i], "--proto") && i + 1 < argc) {
            if (sidplay_set_proto(argv[++i]) != 0) {
                fprintf(stderr, "--proto must be 1step or 2step\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--timer") && i + 1 < argc) {
            if (spfm_timer_set(argv[++i]) != 0) {
                fprintf(stderr, "--timer must be hprec|hybrid|mmtimer|vgmplay\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--flush") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "register"))     s_flush_mode = 1;
            else if (!strcmp(m, "command"))  s_flush_mode = 2;
            else { fprintf(stderr, "--flush must be register|command\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--flush-gap") && i + 1 < argc) {
            s_flush_gap_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_flush_gap_us < 0) {
                fprintf(stderr, "--flush-gap must be >= 0 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            s_pad_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_us <= 0) {
                fprintf(stderr, "--pad must be > 0 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad-duty") && i + 1 < argc) {
            s_pad_duty = atoi(argv[++i]);
            if (s_pad_duty < 1 || s_pad_duty > 100) {
                fprintf(stderr, "--pad-duty must be 1..100 (percent)\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--digi") && i + 1 < argc) {
            s_digi_hz = atoi(argv[++i]);
            if (s_digi_hz < 0) s_digi_hz = 0;
        }
        else if (!strcmp(argv[i], "--pad-lead") && i + 1 < argc) {
            s_pad_lead_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_lead_us < 1000) {
                fprintf(stderr, "--pad-lead must be >= 1 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad-chunk") && i + 1 < argc) {
            s_pad_chunk = (int)(atof(argv[++i]) * 1024.0 + 0.5);
            if (s_pad_chunk < 256) s_pad_chunk = 256;
            if (s_pad_chunk > SPFM_BUF_SIZE) s_pad_chunk = SPFM_BUF_SIZE;
        }
        else if (!strcmp(argv[i], "--pad-window") && i + 1 < argc) {
            s_pad_win_ms = atoi(argv[++i]);
            if (s_pad_win_ms < 0) s_pad_win_ms = 0;
        }
        else if (!strcmp(argv[i], "--pad-local") && i + 1 < argc) {
            s_pad_local = atoi(argv[++i]);
            if (s_pad_local < 10) s_pad_local = 10;
            if (s_pad_local > 100) s_pad_local = 100;
        }
        else if (!strcmp(argv[i], "--pad-lag") && i + 1 < argc) {
            s_pad_lag_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_lag_us < 0) s_pad_lag_us = 0;
        }
        else if (!strcmp(argv[i], "--pad-partial"))      s_pad_fit = 0;
        else if (!strcmp(argv[i], "--pad-min") && i + 1 < argc) {
            s_pad_min_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_min_us < 0) s_pad_min_us = 0;
        }
        else if (!strcmp(argv[i], "--pad-scope") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "all"))        s_pad_scope = 0;
            else if (!strcmp(m, "digi"))  s_pad_scope = 1;
            else { fprintf(stderr, "--pad-scope must be all|digi\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--pad-burst") && i + 1 < argc) {
            s_pad_burst_ms = atoi(argv[++i]);
            if (s_pad_burst_ms < 0) s_pad_burst_ms = 0;
        }
        else if (!strcmp(argv[i], "--pad-always"))              s_pad_gate = 0;
        else if (!strcmp(argv[i], "--no-pad-gate"))             s_pad_gate = 0;
        else if (!strcmp(argv[i], "--pad-rate") && i + 1 < argc) {
            s_pad_rate = atoi(argv[++i]);
            if (s_pad_rate < 1) s_pad_rate = 1;
        }
        else if (!strcmp(argv[i], "--pad-hold") && i + 1 < argc) {
            s_pad_hold_ms = atoi(argv[++i]);
            if (s_pad_hold_ms < 0) s_pad_hold_ms = 0;
        }
        else if (!strcmp(argv[i], "--dedup-vol"))               s_dedup_vol = 1;
        else if (!strcmp(argv[i], "--pace"))                    pace = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        } else path = argv[i];
    }

    if (!path) {
        fprintf(stderr, "usage: sidplay <file.sid|file.sidvgm|file.sidreg> [options]\n"
                        "  --timer hprec|hybrid|mmtimer|vgmplay  wait primitive\n"
                        "  --flush register|command   USB flush granularity\n"
                        "  --flush-gap MS   shortest gap worth a separate USB transfer\n"
                        "                   (default: old rule, 0.09 ms; 0 = per frame)\n"
                        "  --pad MS         reverse of --flush-gap: fill gaps up to MS with\n"
                        "                   NOP bytes so the 1.5 Mbaud clock carries the\n"
                        "                   timing. Implies --dedup-vol. Try 0.3 (digi)\n"
                        "  --pad-duty PCT   cap filler at PCT of the wire (default 75)\n"
                        "  --pad-lead MS    how far the host may run ahead; this is the playback\n"
                        "                   latency, and the underrun margin (default 25)\n"
                        "  --pad-chunk KB   bytes queued before a forced USB flush (default 2)\n"
                        "  --pad-burst MS   leaky-bucket ceiling for the filler (default 200)\n"
                        "  --pad-scope S    all | digi: pad every gap, or only gaps\n"
                        "                   touching $D418 (default all)\n"
                        "  --pad-min MS     never pad a gap shorter than this (default 0)\n"
                        "  --pad-partial    allow half-filled gaps (default: all-or-nothing,\n"
                        "                   which is what keeps the tempo even)\n"
                        "  --pad-lag MS     stop padding once queued wire time runs this\n"
                        "                   far ahead of the music (0 = off). This is the\n"
                        "                   knob that keeps the tempo even under heavy fill.\n"
                        "  --pad-window MS  sliding window for the local filler cap (0 = off).\n"
                        "                   This is the fix for uneven tempo: a global average\n"
                        "                   cannot see a 20 ms burst overdrawing the line.\n"
                        "  --pad-local PCT  max line use inside that window (default 100)\n"
                        "  --digi HZ        resample ONLY the $D418 drum writes onto a\n"
                        "                   1/HZ grid; melody/gate/ADSR untouched.\n"
                        "                   0 = off. 8000 = near-lossless, 4000/2000 = grittier\n"
                        "  --dedup-vol      drop $D418 writes that repeat the latched value\n"
                        "  --proto 1step|2step   write protocol\n"
                        "  --seconds N  --subsong N  --speed X  --no-cache\n"
                        "  .sidvgm/.sidreg replay a recording; .sid is emulated live\n");
        return 1;
    }
    pad_normalize();
    if (speed <= 0) speed = 1.0;

    sid_song_info_t info;
    memset(&info, 0, sizeof info);

    /* A pre-rendered .sidreg / .sidvgm has a fixed length by construction -
     * it is a dump, not a song - so those paths replay the buffer once. A
     * real .sid runs live and forever. */
    if (is_rendered(path)) {
        stream_t st;
        char err[128];
        if (load_stream(path, subsong, seconds > 0 ? seconds : 1e9,
                        use_cache, &st, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        if (st.count == 0) {
            fprintf(stderr, "no register writes in %s\n", path);
            stream_free(&st);
            return 1;
        }
        printf("stream: %s\n%lu writes over %.1f s\n", st.info.name,
               (unsigned long)st.count, st.events[st.count - 1].t_ms / 1000.0);
        fflush(stdout);
        if (hw_connect() != 0) { stream_free(&st); return 1; }
        sid_clear_all_regs(s_opt.slot);
        sid_flush();
        spfm_thread_boost_begin();
        kb_start();
        int rc = replay(st.events, st.count, speed, pace);
        spfm_thread_boost_end();
        hw_mute();
        hw_disconnect();
        stream_free(&st);
        return rc == 0 ? 0 : 1;
    }

    sid_song_info_t probe;
    if (sid_song_probe(path, &probe) != 0) {
        fprintf(stderr, "cannot read header of %s\n", path);
        return 1;
    }
    if (subsong < 0) subsong = probe.start_song ? probe.start_song - 1 : 0;
    if (subsong >= (int)probe.n_songs) subsong = probe.n_songs - 1;

    printf("song: %s / %s\n", probe.name, probe.author);
    if (s_pad_us >= 0 && seconds <= 0) {
        /* Only a *live* .sid is out of reach: it is generated a frame at a
         * time, so the next gap is not known yet. With --seconds the whole
         * span is rendered first and replayed from a finished buffer, which is
         * exactly what padding needs, so leave it alone in that case. */
        fprintf(stderr, "note: --pad needs a finished stream; either play a "
                        ".sidvgm/.sidreg or give --seconds N (the first N "
                        "seconds are rendered, then padded). "
                        "Ignoring --pad for this live .sid\n");
        s_pad_us = -1;
        s_dedup_vol = 0;
    }
    if (seconds > 0)
        printf("playing %.0f s (from the start)\n", seconds);
    else
        printf("playing until interrupted: [n] next-track marker, [q] quit\n");
    printf("subsong %d of %u, speed %.2fx, %s writes\n", subsong, probe.n_songs,
           speed, sidplay_proto_name());
    fflush(stdout);

    if (hw_connect() != 0) return 1;

    /* Start from a known state: clear every register, then let the song's own
     * init writes define the chip. */
    sid_clear_all_regs(s_opt.slot);
    sid_flush();
    spfm_thread_boost_begin();
    kb_start();

    int rc;
    if (seconds > 0) {
        /* Bounded run: render just that much and replay it. Used for quick
         * checks and for `--seconds` given explicitly. */
        stream_t st;
        char err[128];
        if (load_stream(path, subsong, seconds, 0, &st, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            hw_disconnect();
            return 1;
        }
        rc = replay(st.events, st.count, speed, pace);
        stream_free(&st);
    } else {
        rc = stream_play_sid(path, subsong, speed, &info);
    }

    spfm_thread_boost_end();
    hw_mute();
    hw_disconnect();
    return (rc < 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* playlist                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[512];     /* relative to the playlist root, so a recursive walk
                         * keeps the subdirectory ("rsid/x.sidvgm") */
} entry_t;

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

/* The playlist accepts every format `sidplay` can open, so a folder of
 * recorded .sidvgm streams plays just like a folder of .sid files. */
static int is_playable_name(const char *name)
{
    return has_suffix(name, ".sid") || has_suffix(name, ".sidvgm") ||
           has_suffix(name, ".sidreg");
}

static int add_entry(entry_t **list, int *n, int *cap, const char *relname)
{
    if (*n == *cap) {
        int want = *cap ? *cap * 2 : 64;
        entry_t *nl = (entry_t *)realloc(*list, (size_t)want * sizeof(*nl));
        if (!nl) return -1;
        *list = nl;
        *cap = want;
    }
    snprintf((*list)[*n].name, sizeof (*list)[*n].name, "%.511s", relname);
    (*n)++;
    return 0;
}

/* Walk the whole tree. `sid/` and `sidvgm/` both have an rsid/ subdirectory,
 * and listing only the top level silently hid 112 of 124 tunes. */
static int scan_walk(const char *root, const char *rel,
                     entry_t **list, int *n, int *cap)
{
    char pattern[1024];
#ifdef _WIN32
    if (*rel) snprintf(pattern, sizeof pattern, "%s/%s/*", root, rel);
    else      snprintf(pattern, sizeof pattern, "%s/*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return (*n > 0) ? 0 : -1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;

        char child[1024];
        if (*rel) snprintf(child, sizeof child, "%s/%s", rel, fd.cFileName);
        else      snprintf(child, sizeof child, "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_walk(root, child, list, n, cap);
            continue;
        }
        if (!is_playable_name(fd.cFileName)) continue;
        if (add_entry(list, n, cap, child) != 0) { FindClose(h); return -1; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    char full[1024];
    if (*rel) snprintf(full, sizeof full, "%s/%s", root, rel);
    else      snprintf(full, sizeof full, "%s", root);

    DIR *d = opendir(full);
    if (!d) return (*n > 0) ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char child[1024];
        if (*rel) snprintf(child, sizeof child, "%s/%s", rel, de->d_name);
        else      snprintf(child, sizeof child, "%s", de->d_name);

        char childfull[1200];
        snprintf(childfull, sizeof childfull, "%s/%s", root, child);
        struct stat sst;
        if (stat(childfull, &sst) == 0 && S_ISDIR(sst.st_mode)) {
            scan_walk(root, child, list, n, cap);
            continue;
        }
        if (!is_playable_name(de->d_name)) continue;
        if (add_entry(list, n, cap, child) != 0) { closedir(d); return -1; }
    }
    closedir(d);
#endif
    return 0;
}

static int scan_dir(const char *dir, entry_t **out, int *count)
{
    entry_t *list = NULL;
    int n = 0, cap = 0;

    if (scan_walk(dir, "", &list, &n, &cap) != 0 && n == 0) {
        free(list);
        return -1;
    }

    if (n > 1) qsort(list, (size_t)n, sizeof(*list), cmp_entry);
    *out = list;
    *count = n;
    return 0;
}

/* Directory part of an entry ("a/b/c.sidvgm" -> "a/b", "c.sid" -> ""). */
static void entry_dir(const char *name, char *out, size_t len)
{
    const char *slash = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) { out[0] = 0; return; }
    size_t k = (size_t)(slash - name);
    if (k >= len) k = len - 1;
    memcpy(out, name, k);
    out[k] = 0;
}

/* Bounds of the run of entries sharing this entry's directory.
 *
 * The playlist is sorted by relative path, so every file of one folder is
 * contiguous. A recording that has subsongs lives in its own folder
 * (Golden_Axe/Golden_Axe_s01.sidvgm ...), which is what makes "," and "."
 * mean "previous/next subsong" for .sidvgm exactly as they do for .sid. */
static void group_bounds(const entry_t *files, int n, int idx, int *first, int *last)
{
    char here[512], other[512];
    entry_dir(files[idx].name, here, sizeof here);

    int f = idx, l = idx;
    while (f > 0) {
        entry_dir(files[f - 1].name, other, sizeof other);
        if (strcmp(here, other) != 0) break;
        f--;
    }
    while (l + 1 < n) {
        entry_dir(files[l + 1].name, other, sizeof other);
        if (strcmp(here, other) != 0) break;
        l++;
    }
    *first = f;
    *last  = l;
}

static int cmd_playlist(int argc, char **argv)
{
    const char *dir = "sid";
    double seconds = 0.0;          /* 0 = play each track forever */
    double speed = 1.0;
    int start_idx = 0;
    int subsong = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc)    start_idx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (!strcmp(argv[i], "--proto") && i + 1 < argc) {
            if (sidplay_set_proto(argv[++i]) != 0) {
                fprintf(stderr, "--proto must be 1step or 2step\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--timer") && i + 1 < argc) {
            if (spfm_timer_set(argv[++i]) != 0) {
                fprintf(stderr, "--timer must be hprec|hybrid|mmtimer|vgmplay\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--flush") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "register"))     s_flush_mode = 1;
            else if (!strcmp(m, "command"))  s_flush_mode = 2;
            else { fprintf(stderr, "--flush must be register|command\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--flush-gap") && i + 1 < argc) {
            s_flush_gap_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_flush_gap_us < 0) {
                fprintf(stderr, "--flush-gap must be >= 0 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            s_pad_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_us <= 0) {
                fprintf(stderr, "--pad must be > 0 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad-duty") && i + 1 < argc) {
            s_pad_duty = atoi(argv[++i]);
            if (s_pad_duty < 1 || s_pad_duty > 100) {
                fprintf(stderr, "--pad-duty must be 1..100 (percent)\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--digi") && i + 1 < argc) {
            s_digi_hz = atoi(argv[++i]);
            if (s_digi_hz < 0) s_digi_hz = 0;
        }
        else if (!strcmp(argv[i], "--pad-lead") && i + 1 < argc) {
            s_pad_lead_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_lead_us < 1000) {
                fprintf(stderr, "--pad-lead must be >= 1 ms\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pad-chunk") && i + 1 < argc) {
            s_pad_chunk = (int)(atof(argv[++i]) * 1024.0 + 0.5);
            if (s_pad_chunk < 256) s_pad_chunk = 256;
            if (s_pad_chunk > SPFM_BUF_SIZE) s_pad_chunk = SPFM_BUF_SIZE;
        }
        else if (!strcmp(argv[i], "--pad-window") && i + 1 < argc) {
            s_pad_win_ms = atoi(argv[++i]);
            if (s_pad_win_ms < 0) s_pad_win_ms = 0;
        }
        else if (!strcmp(argv[i], "--pad-local") && i + 1 < argc) {
            s_pad_local = atoi(argv[++i]);
            if (s_pad_local < 10) s_pad_local = 10;
            if (s_pad_local > 100) s_pad_local = 100;
        }
        else if (!strcmp(argv[i], "--pad-lag") && i + 1 < argc) {
            s_pad_lag_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_lag_us < 0) s_pad_lag_us = 0;
        }
        else if (!strcmp(argv[i], "--pad-partial"))      s_pad_fit = 0;
        else if (!strcmp(argv[i], "--pad-min") && i + 1 < argc) {
            s_pad_min_us = (int)(atof(argv[++i]) * 1000.0 + 0.5);
            if (s_pad_min_us < 0) s_pad_min_us = 0;
        }
        else if (!strcmp(argv[i], "--pad-scope") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "all"))        s_pad_scope = 0;
            else if (!strcmp(m, "digi"))  s_pad_scope = 1;
            else { fprintf(stderr, "--pad-scope must be all|digi\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--pad-burst") && i + 1 < argc) {
            s_pad_burst_ms = atoi(argv[++i]);
            if (s_pad_burst_ms < 0) s_pad_burst_ms = 0;
        }
        else if (!strcmp(argv[i], "--pad-always"))              s_pad_gate = 0;
        else if (!strcmp(argv[i], "--no-pad-gate"))             s_pad_gate = 0;
        else if (!strcmp(argv[i], "--pad-rate") && i + 1 < argc) {
            s_pad_rate = atoi(argv[++i]);
            if (s_pad_rate < 1) s_pad_rate = 1;
        }
        else if (!strcmp(argv[i], "--pad-hold") && i + 1 < argc) {
            s_pad_hold_ms = atoi(argv[++i]);
            if (s_pad_hold_ms < 0) s_pad_hold_ms = 0;
        }
        else if (!strcmp(argv[i], "--dedup-vol"))               s_dedup_vol = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        } else dir = argv[i];
    }
    pad_normalize();

    entry_t *files = NULL;
    int nfiles = 0;
    if (scan_dir(dir, &files, &nfiles) != 0 || nfiles == 0) {
        fprintf(stderr, "no .sid files in '%s'\n", dir);
        free(files);
        return 1;
    }

    if (hw_connect() != 0) { free(files); return 1; }

    spfm_thread_boost_begin();
    kb_start();

    printf("\n%d files in %s\n", nfiles, dir);
    printf("each track plays until you press a key.  "
           "[n] next   [b] back   [,] prev subsong   [.] next subsong   [q] quit\n");
    printf("write protocol: %s   timer: %s   flush: %s\n\n",
           sidplay_proto_name(), spfm_timer_name(),
           s_flush_mode == 1 ? "register" : "command");

    int idx = start_idx < 0 ? 0 : (start_idx >= nfiles ? nfiles - 1 : start_idx);

    /* `subsong` is the CLI request and applies to a .sid. For a recording the
     * subsong is a file inside the tune's own folder, so the selected index
     * there is the play position instead. */
    int sid_sub = subsong;

    while (idx >= 0 && idx < nfiles) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, files[idx].name);

        int first, last;
        group_bounds(files, nfiles, idx, &first, &last);
        const int gn = last - first + 1;      /* files in this tune's folder */
        const int rendered = is_rendered(path);

        if (rendered) {
            printf("[%d/%d] %s%s\n", idx + 1, nfiles, files[idx].name,
                   gn > 1 ? "" : (is_sidvgm(path) ? "  (sidvgm)" : "  (sidreg)"));
            if (gn > 1)
                printf("        subsong %d of %d, playing...\n",
                       idx - first + 1, gn);
        } else {
            sid_song_info_t probe;
            int nsongs = 1;
            if (sid_song_probe(path, &probe) == 0) {
                nsongs = probe.n_songs ? probe.n_songs : 1;
                if (sid_sub >= nsongs) sid_sub = nsongs - 1;
                printf("[%d/%d] %s\n        %s / %s  (%d subsong%s)\n",
                       idx + 1, nfiles, files[idx].name,
                       probe.name[0] ? probe.name : "?",
                       probe.author[0] ? probe.author : "?",
                       nsongs, nsongs == 1 ? "" : "s");
                printf("        subsong %d, playing...\n", sid_sub);
            } else {
                printf("[%d/%d] %s\n", idx + 1, nfiles, files[idx].name);
            }
        }
        fflush(stdout);

        s_action = 0;
        s_interrupt = 0;

        sid_clear_all_regs(s_opt.slot);
        sid_flush();

        int rc;
        if (seconds > 0 || rendered) {
            /* Bounded mode, and the only mode a recording has: a stream has a
             * fixed end, so it is replayed once (span 1e9 = the whole thing). */
            stream_t st;
            char err[128];
            double span = seconds > 0 ? seconds : 1e9;
            if (load_stream(path, sid_sub, span, 0, &st, err, sizeof err) != 0) {
                fprintf(stderr, "  skip: %s\n", err);
                idx++;
                continue;
            }
            rc = st.count ? replay(st.events, st.count, speed, 1) : 0;
            stream_free(&st);
        } else {
            /* Normal mode: run live, forever, until a key arrives. */
            rc = stream_play_sid(path, sid_sub, speed, NULL);
        }

        hw_mute();

        /* If playback was interrupted, that key decides what happens next;
         * otherwise no key was pressed at all, so advance. */
        int action = s_interrupt ? s_interrupt : kb_take();
        if (action == 'q') break;

        switch (action) {
        /* b = the previous tune, not the previous subsong: stepping back one
         * file inside a 23-subsong folder is never what a listener means. */
        case 'b':
            if (idx <= 0) {
                idx = 0;
            } else {
                int pf, pl;
                group_bounds(files, nfiles, idx - 1, &pf, &pl);
                idx = pf;
            }
            printf("\n[BACK]\n\n");
            break;

        /* Either kind of subsong change: a recording steps through its own
         * folder, a .sid re-extracts the same file with another subsong. */
        case ',':
            if (rendered && gn > 1) {
                idx = idx > first ? idx - 1 : first;
                printf("\n[SUBSONG %d/%d]\n\n", idx - first + 1, gn);
            } else {
                sid_sub = sid_sub > 0 ? sid_sub - 1 : 0;
                printf("\n[SUBSONG %d]\n\n", sid_sub);
            }
            break;

        case '.':
            if (rendered && gn > 1) {
                idx = idx < last ? idx + 1 : last;
                printf("\n[SUBSONG %d/%d]\n\n", idx - first + 1, gn);
            } else {
                sid_sub++;
                printf("\n[SUBSONG %d]\n\n", sid_sub);
            }
            break;

        /* n = leave this tune behind; for a folder that means all its
         * subsongs, otherwise one file. */
        case 'n':
            idx = (rendered && gn > 1) ? last + 1 : idx + 1;
            break;

        default:
            idx++;
            break;
        }
        (void)rc;
        spfm_delay_ms(200);
    }

    printf("\nplaylist end\n");
    spfm_thread_boost_end();
    hw_disconnect();
    free(files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* write protocol                                                      */
/* ------------------------------------------------------------------ */

/* The two protocols that matter for the "is one step enough?" experiment.
 * Everything else in sid_write_mode_t is a probe for a specific failure
 * (split to separate the two phases, *-soft to isolate USB pacing); these two
 * are the honest candidates for production. */
/* Set when the caller named a protocol (or a write mode) on the command line.
 * Without it, apply_common() would keep overwriting an explicit choice with
 * the playback default. */
static int s_proto_explicit = 0;

int sidplay_set_proto(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "2step") || !strcmp(name, "two-phase") ||
        !strcmp(name, "two_phase") || !strcmp(name, "2")) {
        sid_set_write_mode(SID_WRITE_TWO_PHASE);
        s_proto_explicit = 1;
        return 0;
    }
    if (!strcmp(name, "1step") || !strcmp(name, "path-a") ||
        !strcmp(name, "burst") || !strcmp(name, "1")) {
        sid_set_write_mode(SID_WRITE_BURST);
        s_proto_explicit = 1;
        return 0;
    }
    return -1;
}

void sidplay_set_proto_explicit(void)
{
    s_proto_explicit = 1;
}

/* Read back from the driver rather than remembering a name, so --write-mode
 * and --proto cannot disagree about what is actually on the bus. */
const char *sidplay_proto_name(void)
{
    switch (sid_get_write_mode()) {
    case SID_WRITE_BURST:          return "1step";
    case SID_WRITE_TWO_PHASE:      return "2step";
    case SID_WRITE_TWO_PHASE_NOP:  return "2step+nop";
    case SID_WRITE_PATH_A_SOFT:    return "1step+flush";
    case SID_WRITE_SPLIT:          return "split";
    default:                       return "?";
    }
}

/* ------------------------------------------------------------------ */
/* dispatcher                                                          */
/* ------------------------------------------------------------------ */

/* Each entry point takes the shared options, applies the ones that belong to
 * the driver, then runs its own command. Keeping them separate lets sidtest's
 * dispatcher stay a table of calls rather than a chain of string compares. */
static void apply_common(const sidplay_opts_t *opts)
{
    if (!opts) return;
    s_opt = *opts;
    if (s_opt.write_mode > 0) {
        sid_set_write_mode((sid_write_mode_t)s_opt.write_mode);
        s_proto_explicit = 1;
    }
    /* Playback default: one Path A command per register write. Measured on
     * this board, 1step played several tunes that 2step mangled, which fits
     * the async model - the SID latches on /CS and R/W, so there is nothing
     * for a host-controlled gap to accomplish. 2step stays one flag away
     * because it is what the earlier wiring needed. */
    if (!s_proto_explicit)
        sid_set_write_mode(SID_WRITE_BURST);
}

int sidplay_do_play(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_play(argc, argv);
}

int sidplay_do_extract(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_extract(argc, argv);
}

int sidplay_do_info(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_info(argc, argv);
}

int sidplay_do_list(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_playlist(argc, argv);
}
