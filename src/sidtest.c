/*
 * sidtest.c - console smoke test for the SPFM SID 6581 driver.
 *
 * Every test below follows the register usage of the reference AVR firmware
 * (arduino_sid/midi.c): triangle/saw/pulse/noise selected through 0x04,
 * AD = 0x60 (A=6,D=0), SR = 0xF2 (S=15,R=2), PWM 0x500, filter 0x17 = 0xF7,
 * 0x18 = 0x1F.
 *
 * Usage:
 *   sidtest [options] <command> [args]
 *
 * Options:
 *   --slot N        SPFM slot, default 3 (last slot of RE-SPFM)
 *   --device N      FTDI device index, default 0
 *   --dry-run       do not touch hardware; decode the byte stream instead
 *   --verbose       echo every register write
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "spfm_transport.h"
#include "sid6581.h"
#include "sidplay.h"

/* ------------------------------------------------------------------ */
/* midi.c defaults                                                     */
/* ------------------------------------------------------------------ */
#define REF_WAVE       SID_CTL_PULSE      /* midi.c: VOICE 2 = square */
#define REF_AD         0x60               /* ATTACK 6, DECAY 0        */
#define REF_SR         0xF2               /* SUSTAIN 15, RELEASE 2    */
#define REF_PW         0x500              /* PULSE_WIDTH              */
#define REF_FILTER_REG 0xF7               /* RESONANCE 15, LP path    */
#define REF_MODE       0x1F               /* LP filter + volume 15    */

static uint8_t g_slot    = SID_DEFAULT_SLOT;
static int     g_device  = 0;
static int     g_dry_run = 0;
static int     g_verbose = 0;

static const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void note_name(uint8_t n, char *buf, size_t len)
{
    snprintf(buf, len, "%s%d", NOTE_NAMES[n % 12], (int)(n / 12) - 1);
}

/* ------------------------------------------------------------------ */
/* write hook                                                          */
/* ------------------------------------------------------------------ */
static void hook_print(uint8_t slot, uint8_t reg, uint8_t data, void *user)
{
    (void)user;
    printf("      SID[%u] 0x%02X <- 0x%02X\n", (unsigned)slot,
           (unsigned)reg, (unsigned)data);
}

/* ------------------------------------------------------------------ */
/* helper: connect                                                     */
/* ------------------------------------------------------------------ */
static int connect_device(void)
{
    if (g_dry_run) {
        spfm_dry_run_enable(true);
        printf("dry-run: hardware not touched, byte stream is decoded instead\n");
        return 0;
    }

    uint32_t ids[16];
    char descs[16][64];
    int n = spfm_list_devices(ids, 16, descs);
    if (n <= 0) {
        fprintf(stderr, "no FTDI devices found (is the SPFM plugged in?)\n");
        return -1;
    }
    if (g_device >= n) {
        fprintf(stderr, "--device %d out of range, %d device(s) present\n",
                g_device, n);
        return -1;
    }
    printf("device %d: %s\n", g_device, descs[g_device]);

    if (spfm_open(g_device) != 0) return -1;
    return 0;
}

static void disconnect(void)
{
    if (!g_dry_run) spfm_close();
}

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */
static int cmd_list(void)
{
    uint32_t ids[16];
    char descs[16][64];
    int n = spfm_list_devices(ids, 16, descs);
    if (n < 0) {
        fprintf(stderr, "FT_CreateDeviceInfoList failed\n");
        return 1;
    }
    if (n == 0) {
        printf("no FTDI devices\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        printf("  [%d] %s\n", i, descs[i]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */
static int cmd_init(int argc, char **argv)
{
    uint8_t vol = 15;
    if (argc > 0) vol = (uint8_t)atoi(argv[0]);

    if (connect_device() != 0) return 1;

    if (!g_dry_run) {
        printf("sending global reset (0xFE)\n");
        spfm_send_reset();
        printf("(note: this resets every chip on every slot, not just slot %u)\n",
               (unsigned)g_slot);
    }

    printf("initialising SID on slot %u, volume %u\n",
           (unsigned)g_slot, (unsigned)vol);
    sid_init(g_slot, vol);

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
        printf("total %lu bytes\n", (unsigned long)spfm_capture_size());
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* raw register write                                                  */
/* ------------------------------------------------------------------ */
static int cmd_reg(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sidtest reg <reg> <data> [<reg> <data> ...]\n");
        return 1;
    }
    if (argc % 2) {
        fprintf(stderr, "reg/data must come in pairs\n");
        return 1;
    }
    if (connect_device() != 0) return 1;

    for (int i = 0; i < argc; i += 2) {
        long r = strtol(argv[i], NULL, 0);
        long d = strtol(argv[i + 1], NULL, 0);
        if (r < 0 || r > 0x18) {
            fprintf(stderr, "register 0x%lX outside 0x00..0x18\n", r);
            disconnect();
            return 1;
        }
        sid_write(g_slot, (uint8_t)r, (uint8_t)(d & 0xFF));
        printf("0x%02lX <- 0x%02lX\n", r, d & 0xFF);
    }
    sid_flush();
    if (g_dry_run) spfm_capture_dump(stdout);
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* shared: sleep helper                                                */
/* ------------------------------------------------------------------ */
static void sleep_ms(uint32_t ms)
{
    spfm_delay_ms(ms);
}

/* ------------------------------------------------------------------ */
/* scale                                                               */
/* ------------------------------------------------------------------ */
static int cmd_scale(int argc, char **argv)
{
    uint8_t wave = REF_WAVE;
    uint8_t start = 60;         /* C4 */
    uint32_t step_ms = 400;
    int use_filter = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wave") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "tri"))        wave = SID_CTL_TRI;
            else if (!strcmp(w, "saw"))   wave = SID_CTL_SAW;
            else if (!strcmp(w, "pulse")) wave = SID_CTL_PULSE;
            else if (!strcmp(w, "noise")) wave = SID_CTL_NOISE;
            else { fprintf(stderr, "unknown wave '%s'\n", w); return 1; }
        } else if (!strcmp(argv[i], "--start") && i + 1 < argc) {
            start = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--step") && i + 1 < argc) {
            step_ms = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--filter")) {
            use_filter = 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    if (use_filter) {
        /* midi.c's exact filter setup: resonance 15, all voices routed to the
         * low-pass, cutoff around the middle of the range. */
        sid_write(g_slot, SID_REG_FC_HI, 0x3E);
        sid_write(g_slot, SID_REG_FC_LO, 0x00);
        sid_write(g_slot, SID_REG_RES_FILT, REF_FILTER_REG);
        sid_write(g_slot, SID_REG_MODE_VOL, REF_MODE);
        sid_flush();
    }

    static const int degrees[8] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    for (int i = 0; i < 8; i++) {
        uint8_t n = (uint8_t)(start + degrees[i]);
        char nm[8];
        note_name(n, nm, sizeof nm);
        printf("note %-4s (midi %3u) freq_reg %5u\n",
               nm, (unsigned)n, (unsigned)sid_freq_from_note(n));

        /* Same call shape as `tone`, which is the path already proven to
         * track pitch correctly: freq lo/hi, AD, SR, then control with gate.
         * Pulse width is set afterwards so it cannot disturb the note start. */
        sid_note_on(g_slot, 0, n, wave, REF_AD, REF_SR);
        if (wave == SID_CTL_PULSE)
            sid_set_pulse(g_slot, 0, REF_PW);
        sid_flush();
        sleep_ms(step_ms);

        sid_note_off(g_slot, 0);
        sid_flush();
        sleep_ms(120);
    }

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* chord: all three voices at once                                     */
/* ------------------------------------------------------------------ */
static int cmd_chord(int argc, char **argv)
{
    uint8_t wave = REF_WAVE;
    uint32_t step_ms = 1200;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--step") && i + 1 < argc) step_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wave") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "tri"))        wave = SID_CTL_TRI;
            else if (!strcmp(w, "saw"))   wave = SID_CTL_SAW;
            else if (!strcmp(w, "pulse")) wave = SID_CTL_PULSE;
            else if (!strcmp(w, "noise")) wave = SID_CTL_NOISE;
        }
    }

    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    static const uint8_t chords[][3] = {
        { 60, 64, 67 },   /* C major   */
        { 57, 60, 64 },   /* A minor   */
        { 53, 57, 60 },   /* F major   */
        { 55, 59, 62 },   /* G major   */
        { 60, 64, 67 },   /* C major   */
    };
    static const char *names[] = { "C", "Am", "F", "G", "C" };
    const int nchords = (int)(sizeof chords / sizeof chords[0]);

    for (int c = 0; c < nchords; c++) {
        printf("chord %-3s : %u %u %u\n", names[c],
               (unsigned)chords[c][0], (unsigned)chords[c][1],
               (unsigned)chords[c][2]);
        for (uint8_t v = 0; v < 3; v++) {
            sid_note_on(g_slot, v, chords[c][v], wave, REF_AD, REF_SR);
            sid_set_pulse(g_slot, v, REF_PW);
        }
        sid_flush();
        sleep_ms(step_ms);
        for (uint8_t v = 0; v < 3; v++) sid_note_off(g_slot, v);
        sid_flush();
        sleep_ms(150);
    }

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* music - three voices: pulse melody + triangle bass + noise drums    */
/* ------------------------------------------------------------------ */

/* One score step: a MIDI note (0 = rest) held for `ticks` sixteenths. */
typedef struct {
    uint8_t note;
    uint8_t ticks;
} mus_step_t;

typedef struct {
    const char       *name;
    const char       *title;
    const mus_step_t *melody;
    const mus_step_t *bass;
} mus_song_t;

/* Envelopes. SID ADSR digits are A/D/S/R, each 0..15.
 *
 * "Piano" feel comes from sustain = 0: the note decays to silence on its own
 * (decay time) and the key-up only shortens the tail (release). Without that,
 * sustain = 15 makes every note hold at full volume until the next one, which
 * is the "infinite organ" sound.
 *
 * AD = (attack<<4) | decay, SR = (sustain<<4) | release.
 * Decay index 8 = 300 ms, 9 = 750 ms, 7 = 240 ms (see sid_adsr_decay_ms). */
#define MUS_AD_MEL     0x08   /* A=0 (instant), D=8 (300 ms decay)   */
#define MUS_SR_MEL     0x06   /* S=0 (fade out), R=6 (204 ms release)*/
#define MUS_PW_MEL     0x600

#define MUS_AD_BASS    0x0A   /* A=0, D=10 (1500 ms) -- bass notes are
                               * held for whole bars, so they need a longer
                               * decay than the melody's 300 ms */
#define MUS_SR_BASS    0x06   /* S=0 (fade out), R=6 (204 ms)         */

/* Percussion: attack 0 so the transient is instant, decay sets how long the
 * hit rings. 240 ms is enough to read as a drum instead of a click. */
#define MUS_AD_DRUM    0x07   /* A=0, D=7 (240 ms)                    */
#define MUS_SR_DRUM    0x00   /* S=0, R=0                             */

/* --- Twinkle Twinkle Little Star, C major, 128 ticks ---------------- */
static const mus_step_t MUS_TWINKLE_MEL[] = {
    { 60,4 },{ 60,4 },{ 67,4 },{ 67,4 },   /* C C G G   */
    { 69,4 },{ 69,4 },{ 67,8 },            /* A A G     */
    { 65,4 },{ 65,4 },{ 64,4 },{ 64,4 },   /* F F E E   */
    { 62,4 },{ 62,4 },{ 60,8 },            /* D D C     */
    { 67,4 },{ 67,4 },{ 65,4 },{ 65,4 },   /* G G F F   */
    { 64,4 },{ 64,4 },{ 62,8 },            /* E E D     */
    { 67,4 },{ 67,4 },{ 65,4 },{ 65,4 },   /* G G F F   */
    { 64,4 },{ 64,4 },{ 62,8 },            /* E E D     */
    { 60,4 },{ 60,4 },{ 67,4 },{ 67,4 },   /* C C G G   */
    { 69,4 },{ 69,4 },{ 67,8 },            /* A A G     */
    { 65,4 },{ 65,4 },{ 64,4 },{ 64,4 },   /* F F E E   */
    { 62,4 },{ 62,4 },{ 60,8 },            /* D D C     */
    { 0,0 }
};
static const mus_step_t MUS_TWINKLE_BASS[] = {
    /* one chord per half line: 12 x 16 = 192, matching the melody */
    { 48,16 },{ 55,16 },   /* C  G   (C C G G / A A G)   */
    { 53,16 },{ 48,16 },   /* F  C   (F F E E / D D C)   */
    { 48,16 },{ 55,16 },   /* C  G   (G G F F / E E D)   */
    { 48,16 },{ 55,16 },   /* C  G                       */
    { 48,16 },{ 55,16 },   /* C  G                       */
    { 53,16 },{ 48,16 },   /* F  C                       */
    { 0,0 }
};

/* --- Ode to Joy (Beethoven), C major, 128 ticks --------------------- */
static const mus_step_t MUS_ODE_MEL[] = {
    { 64,4 },{ 64,4 },{ 65,4 },{ 67,4 },   /* E E F G */
    { 67,4 },{ 65,4 },{ 64,4 },{ 62,4 },   /* G F E D */
    { 60,4 },{ 60,4 },{ 62,4 },{ 64,4 },   /* C C D E */
    { 64,4 },{ 62,4 },{ 62,8 },            /* E D D   */
    { 64,4 },{ 64,4 },{ 65,4 },{ 67,4 },   /* E E F G */
    { 67,4 },{ 65,4 },{ 64,4 },{ 62,4 },   /* G F E D */
    { 60,4 },{ 60,4 },{ 62,4 },{ 64,4 },   /* C C D E */
    { 62,4 },{ 60,4 },{ 60,8 },            /* D C C   */
    { 0,0 }
};
static const mus_step_t MUS_ODE_BASS[] = {
    /* one note per line of 16: 8 x 16 = 128 */
    { 48,16 },{ 55,16 },{ 48,16 },{ 55,16 },
    { 48,16 },{ 55,16 },{ 48,16 },{ 55,16 },
    { 0,0 }
};

/* --- Happy Birthday, C major, 66 ticks ------------------------------ */
static const mus_step_t MUS_BDAY_MEL[] = {
    { 67,2 },{ 67,2 },{ 69,4 },{ 67,4 },{ 72,4 },{ 71,8 },
    { 67,2 },{ 67,2 },{ 69,4 },{ 67,4 },{ 74,4 },{ 72,8 },
    { 67,2 },{ 67,2 },{ 79,4 },{ 76,4 },{ 72,4 },{ 71,6 },{ 69,2 },
    { 77,2 },{ 77,2 },{ 76,4 },{ 72,4 },{ 74,4 },{ 72,8 },
    { 0,0 }
};
static const mus_step_t MUS_BDAY_BASS[] = {
    /* one chord per phrase: 4 x 24 = 96 */
    { 48,24 },   /* C    (C C D F E C)     */
    { 55,24 },   /* G    (C C D G E C)     */
    { 48,24 },   /* C    (C C C' A F E D)  */
    { 55,24 },   /* G    (B B A F G E C)   */
    { 0,0 }
};

/* --- Bach, transcribed from arduino_sid/Source/SID_Bach ------------
 * Original note table (OFFS = 36, i.e. C2..C3). Transposed +12 here so the
 * melody sits above the triangle bass instead of colliding with it. */
static const mus_step_t MUS_BACH_MEL[] = {
    { 48,2 },{ 50,2 },{ 51,2 },{ 53,2 },{ 55,2 },{ 56,2 },{ 55,2 },{ 53,2 },
    { 51,2 },{ 50,2 },{ 51,2 },{ 53,2 },{ 51,2 },{ 50,2 },{ 48,2 },{ 60,2 },
    { 59,2 },{ 60,2 },{ 56,2 },{ 55,2 },{ 53,2 },{ 60,2 },{ 59,2 },{ 60,2 },
    { 55,2 },{ 53,2 },{ 51,2 },{ 60,2 },{ 59,2 },{ 60,2 },{ 53,2 },{ 55,2 },
    { 56,2 },{ 50,2 },{ 55,2 },{ 53,2 },{ 51,2 },{ 0,8 },
};
static const mus_step_t MUS_BACH_BASS[] = {
    /* one root per 16 ticks, walking C - F - G - C - F */
    { 36,16 },{ 41,16 },{ 43,16 },{ 36,16 },{ 41,10 },
    { 0,0 }
};

static const mus_song_t MUS_SONGS[] = {
    { "twinkle",  "Twinkle Twinkle Little Star", MUS_TWINKLE_MEL, MUS_TWINKLE_BASS },
    { "ode",      "Ode to Joy",                  MUS_ODE_MEL,     MUS_ODE_BASS     },
    { "birthday", "Happy Birthday",              MUS_BDAY_MEL,    MUS_BDAY_BASS    },
    { "bach",     "Bach (SID_Bach transcription)", MUS_BACH_MEL,    MUS_BACH_BASS    },
};
#define MUS_COUNT ((int)(sizeof MUS_SONGS / sizeof MUS_SONGS[0]))

/* Restart a voice on a fresh note. The envelope only restarts on a 0->1 gate
 * edge, so the gate has to drop first or two identical notes in a row would
 * be heard as one long note. */
static void mus_note_start(uint8_t voice, uint8_t note, uint8_t wave,
                           uint8_t ad, uint8_t sr)
{
    uint8_t base = (uint8_t)(voice * 7);
    sid_write(g_slot, (uint8_t)(base + SID_REG_CONTROL),
              (uint8_t)(wave & SID_CTL_WAVE_MASK));
    sid_note_on(g_slot, voice, note, wave, ad, sr);
}

/* Noise percussion on voice 3.
 *
 * The noise generator is clocked by the voice frequency, so a LOW frequency
 * gives a dark rumble (kick) and a HIGH one gives bright hiss (hat).
 *
 * The envelope is what makes it read as a drum rather than a click: attack 0
 * for an instant transient, sustain 0 so it decays by itself, and a decay
 * long enough to be heard (kick 300 ms, snare 240 ms, hat 168 ms). Short
 * decays were what made the previous version sound like interference. */
static void mus_drum_hit(uint16_t freq_reg, uint8_t decay_idx, uint8_t release)
{
    uint8_t base = 0x0E;
    uint8_t ad = (uint8_t)(decay_idx & 0x0F);          /* A=0, D=decay_idx */
    uint8_t sr = (uint8_t)(release & 0x0F);            /* S=0, R=release   */

    sid_write(g_slot, (uint8_t)(base + SID_REG_FREQ_LO), (uint8_t)(freq_reg & 0xFF));
    sid_write(g_slot, (uint8_t)(base + SID_REG_FREQ_HI), (uint8_t)(freq_reg >> 8));
    sid_write(g_slot, (uint8_t)(base + SID_REG_ATTACK_DECAY), ad);
    sid_write(g_slot, (uint8_t)(base + SID_REG_SUSTAIN_REL), sr);
    /* Gate down first, then gate up: only the 0->1 edge restarts the ADSR. */
    sid_write(g_slot, (uint8_t)(base + SID_REG_CONTROL), SID_CTL_NOISE);
    sid_write(g_slot, (uint8_t)(base + SID_REG_CONTROL),
              (uint8_t)(SID_CTL_NOISE | SID_CTL_GATE));
}

static int cmd_music(int argc, char **argv)
{
    const char *song_name = "twinkle";
    uint32_t bpm   = 120;
    uint32_t loops = 1;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--song") && i + 1 < argc) {
            song_name = argv[++i];
        } else if (!strcmp(argv[i], "--bpm") && i + 1 < argc) {
            bpm = (uint32_t)atoi(argv[++i]);
            if (bpm < 20 || bpm > 400) {
                fprintf(stderr, "bpm must be 20..400\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--loops") && i + 1 < argc) {
            loops = (uint32_t)atoi(argv[++i]);
            if (loops == 0) loops = 1;
        } else if (!strcmp(argv[i], "--list")) {
            for (int k = 0; k < MUS_COUNT; k++)
                printf("  %-9s %s\n", MUS_SONGS[k].name, MUS_SONGS[k].title);
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    const mus_song_t *sc = NULL;
    for (int k = 0; k < MUS_COUNT; k++) {
        if (!strcmp(MUS_SONGS[k].name, song_name)) { sc = &MUS_SONGS[k]; break; }
    }
    if (!sc) {
        fprintf(stderr, "unknown song '%s' (try --list)\n", song_name);
        return 1;
    }

    /* Song length comes from the melody; the bass array is written to match. */
    uint32_t total = 0;
    for (size_t i = 0; sc->melody[i].ticks; i++) total += sc->melody[i].ticks;

    /* One sixteenth note. Integer maths is fine at these tempos. */
    uint32_t tick_ms = 60000u / (bpm * 4u);
    if (tick_ms == 0) tick_ms = 1;

    printf("song: %s\n", sc->title);
    printf("bpm=%lu  tick=%lu ms  ticks=%lu  loops=%lu\n",
           (unsigned long)bpm, (unsigned long)tick_ms,
           (unsigned long)total, (unsigned long)loops);
    printf("voices: 1=pulse melody  2=triangle bass  3=noise drums\n");

    if (connect_device() != 0) return 1;

    sid_init(g_slot, 15);
    /* No filter: routing all three voices into a low-pass would swallow the
     * noise channel and make the drums inaudible. */
    sid_set_filter(g_slot, 0x7FF, 0x00, 0x00);
    sid_set_pulse(g_slot, 0, MUS_PW_MEL);
    sid_flush();

    for (uint32_t loop = 0; loop < loops; loop++) {
        size_t   mi = 0, bi = 0;
        uint32_t mleft = 0, bleft = 0;
        uint8_t  last_mel = 0;

        if (loops > 1) {
            printf("--- loop %lu/%lu ---\n",
                   (unsigned long)(loop + 1), (unsigned long)loops);
        }

        for (uint32_t t = 0; t < total; t++) {
            /* Release one tick early so consecutive notes are articulated
             * instead of running together. With sustain 0 the note has already
             * decayed, but dropping the gate still sharpens the attack of the
             * next one. */
            if (mleft == 1 && sc->melody[mi].ticks) sid_note_off(g_slot, 0);
            if (bleft == 1 && sc->bass[bi].ticks)   sid_note_off(g_slot, 1);

            /* ---- melody (voice 1, pulse) ---- */
            if (mleft == 0 && sc->melody[mi].ticks) {
                const mus_step_t *st = &sc->melody[mi++];
                last_mel = st->note;
                if (st->note) {
                    mus_note_start(0, st->note, SID_CTL_PULSE,
                                   MUS_AD_MEL, MUS_SR_MEL);
                } else {
                    sid_note_off(g_slot, 0);
                }
                mleft = st->ticks;
            }

            /* ---- bass (voice 2, triangle) ---- */
            if (bleft == 0 && sc->bass[bi].ticks) {
                const mus_step_t *st = &sc->bass[bi++];
                if (st->note) {
                    mus_note_start(1, st->note, SID_CTL_TRI,
                                   MUS_AD_BASS, MUS_SR_BASS);
                } else {
                    sid_note_off(g_slot, 1);
                }
                bleft = st->ticks;
            }

            /* ---- drums (voice 3, noise) ----
             * decay index: 8 = 300 ms, 7 = 240 ms, 5 = 168 ms, 3 = 72 ms
             * (see sid_adsr_decay_ms[]). Longer than a tick so the hit is
             * heard as a drum, short enough not to smear into the next one. */
            if ((t % 8) == 0) {                    /* kick, 4-on-the-floor */
                mus_drum_hit(0x0400, 8, 3);
            } else if ((t % 8) == 4) {             /* snare on the backbeat */
                mus_drum_hit(0x0E00, 7, 3);
            } else if ((t % 4) == 2) {             /* hi-hat on the offbeat  */
                mus_drum_hit(0x2400, 5, 2);
            }

            sid_flush();

            if ((t % 16) == 0) {
                char nm[8];
                if (last_mel) note_name(last_mel, nm, sizeof nm);
                else          snprintf(nm, sizeof nm, "rest");
                printf("bar %2lu  %-4s", (unsigned long)(t / 16) + 1, nm);
                fflush(stdout);
            }
            if ((t % 16) == 15) printf("\n");

            sleep_ms(tick_ms);
            if (mleft) mleft--;
            if (bleft) bleft--;
        }

        /* Silence everything between loops and at the end. */
        sid_note_off(g_slot, 0);
        sid_note_off(g_slot, 1);
        sid_write(g_slot, (uint8_t)(0x0E + SID_REG_CONTROL), 0x00);
        sid_flush();
        sleep_ms(300);
    }
    printf("\n");

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* noise                                                               */
/* ------------------------------------------------------------------ */
static int cmd_noise(int argc, char **argv)
{
    uint32_t step_ms = 250;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--step") && i + 1 < argc) step_ms = (uint32_t)atoi(argv[++i]);

    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    /* Noise pitch comes from the voice frequency register, so sweeping the
     * frequency sweeps the noise. */
    uint16_t freqs[] = { 0x0100, 0x0300, 0x0600, 0x0C00, 0x1800, 0x3000, 0x6000 };
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; i++) {
        printf("noise freq 0x%04X\n", (unsigned)freqs[i]);
        sid_voice_config(g_slot, 0, freqs[i], REF_PW, SID_CTL_NOISE, 0x00, 0xF0);
        sid_write(g_slot, SID_REG_CONTROL, SID_CTL_NOISE | SID_CTL_GATE);
        sid_flush();
        sleep_ms(step_ms);
    }
    sid_note_kill(g_slot, 0);
    sid_flush();

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* pwm sweep                                                           */
/* ------------------------------------------------------------------ */
static int cmd_pwm(int argc, char **argv)
{
    uint32_t step_ms = 40;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--step") && i + 1 < argc) step_ms = (uint32_t)atoi(argv[++i]);

    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    sid_note_on(g_slot, 0, 60, SID_CTL_PULSE, REF_AD, 0xF0);
    sid_flush();
    printf("PWM sweep 0..4095 on voice 1 (A4)\n");
    for (uint16_t pw = 0; pw < SID_PW_MAX; pw += 64) {
        sid_set_pulse(g_slot, 0, pw);
        if ((pw & 0x3FF) == 0) {
            sid_flush();
            sleep_ms(step_ms);
        }
    }
    sid_flush();
    sleep_ms(300);
    sid_note_off(g_slot, 0);
    sid_flush();

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* filter sweep                                                        */
/* ------------------------------------------------------------------ */
static int cmd_filter(int argc, char **argv)
{
    uint32_t step_ms = 60;
    uint8_t res = 15;
    uint8_t mode = SID_MODE_LP;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--step") && i + 1 < argc) step_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) res = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "lp"))      mode = SID_MODE_LP;
            else if (!strcmp(m, "bp")) mode = SID_MODE_BP;
            else if (!strcmp(m, "hp")) mode = SID_MODE_HP;
            else { fprintf(stderr, "mode must be lp/bp/hp\n"); return 1; }
        }
    }

    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    /* Sawtooth is the classic filter-sweep source: dense harmonics. */
    sid_note_on(g_slot, 0, 48, SID_CTL_SAW, 0x00, 0xF0);
    sid_flush();

    sid_set_filter(g_slot, 0, res,
                   SID_FILT_V1 | SID_FILT_V2 | SID_FILT_V3);
    sid_set_mode(g_slot, (uint8_t)(mode | SID_MODE_V3OFF));
    sid_flush();

    printf("filter sweep 0..2047, resonance %u\n", (unsigned)res);
    for (uint16_t c = 0; c <= 0x7FF; c += 0x20) {
        sid_set_filter_cutoff(g_slot, c);
        if ((c & 0xFF) == 0) {
            sid_flush();
            sleep_ms(step_ms);
        }
    }
    sid_flush();
    sleep_ms(200);

    sid_note_off(g_slot, 0);
    sid_flush();

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* tone - one sustained note, so the operator can judge by ear          */
/* ------------------------------------------------------------------ */
static int cmd_tone(int argc, char **argv)
{
    uint8_t note = 69;          /* A4 */
    uint8_t wave = SID_CTL_TRI;
    uint32_t secs = 3;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--note") && i + 1 < argc) note = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wave") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "tri"))        wave = SID_CTL_TRI;
            else if (!strcmp(w, "saw"))   wave = SID_CTL_SAW;
            else if (!strcmp(w, "pulse")) wave = SID_CTL_PULSE;
            else if (!strcmp(w, "noise")) wave = SID_CTL_NOISE;
            else { fprintf(stderr, "unknown wave '%s'\n", w); return 1; }
        }
    }

    if (connect_device() != 0) return 1;

    const char *mode_name =
        sid_get_write_mode() == SID_WRITE_TWO_PHASE     ? "two-phase" :
        sid_get_write_mode() == SID_WRITE_TWO_PHASE_NOP ? "two-phase-nop" :
        sid_get_write_mode() == SID_WRITE_PATH_A_SOFT   ? "path-a-soft" :
        sid_get_write_mode() == SID_WRITE_SPLIT         ? "split" : "burst";

    printf("mode=%s gap=%lu rand=%lu addr=%lu data=%lu delay=%luus repeat=%lu"
           "  note=%u wave=0x%02X %lus\n",
           mode_name, (unsigned long)sid_get_phase_gap(),
           (unsigned long)sid_get_gap_random(),
           (unsigned long)sid_get_addr_repeat(),
           (unsigned long)sid_get_data_repeat(),
           (unsigned long)sid_get_write_delay(),
           (unsigned long)sid_get_write_repeat(),
           (unsigned)note, (unsigned)wave, (unsigned long)secs);

    sid_init(g_slot, 15);
    sid_note_on(g_slot, 0, note, wave, 0x00, 0xF0);   /* instant A, full S */
    sid_flush();

    /* Keep the note alive with periodic refreshes: if the chip is only
     * catching a fraction of the writes, repeated gate writes slowly train it
     * into a stable state instead of leaving it half-programmed. */
    for (uint32_t t = 0; t < secs * 10; t++) {
        sleep_ms(100);
        printf(".");
        fflush(stdout);
        if ((t % 5) == 4) {
            sid_set_freq(g_slot, 0, sid_freq_from_note(note));
            sid_write(g_slot, 4, (uint8_t)(wave | SID_CTL_GATE));
            sid_flush();
        }
    }
    printf("\n");

    sid_note_off(g_slot, 0);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* probe - walk configurations, each with a distinct pitch            */
/* ------------------------------------------------------------------ */
static int cmd_probe(void)
{
    struct cfg {
        const char *label;
        sid_write_mode_t mode;
        uint32_t delay_us, repeat, gap;
        uint8_t note;
    } cfgs[] = {
        { "two-phase  delay=90  repeat=1",  SID_WRITE_TWO_PHASE,     90,  1, 0, 57 },
        { "two-phase  delay=90  repeat=8",  SID_WRITE_TWO_PHASE,     90,  8, 0, 60 },
        { "two-phase  delay=90  repeat=32", SID_WRITE_TWO_PHASE,     90, 32, 0, 64 },
        { "two-phase  delay=0   repeat=64", SID_WRITE_TWO_PHASE,      0, 64, 0, 67 },
        { "two-phase  delay=500 repeat=1",  SID_WRITE_TWO_PHASE,    500,  1, 0, 72 },
        { "two-phase-nop gap=8  repeat=1",  SID_WRITE_TWO_PHASE_NOP,  0,  1, 8, 76 },
        { "path-a              repeat=1",   SID_WRITE_BURST,        90,  1, 0, 79 },
    };
    const int n = (int)(sizeof cfgs / sizeof cfgs[0]);

    if (connect_device() != 0) return 1;

    for (int i = 0; i < n; i++) {
        sid_set_write_mode(cfgs[i].mode);
        sid_set_write_delay(cfgs[i].delay_us);
        sid_set_write_repeat(cfgs[i].repeat);
        sid_set_phase_gap(cfgs[i].gap);

        printf("[%d/%d] %-34s note %3u ... ", i + 1, n, cfgs[i].label,
               (unsigned)cfgs[i].note);
        fflush(stdout);

        sid_init(g_slot, 15);
        sid_note_on(g_slot, 0, cfgs[i].note, SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(2000);
        sid_note_off(g_slot, 0);
        sid_flush();
        sleep_ms(400);
        printf("done\n");
    }

    printf("\nwhich one produced a steady tone? report the number above.\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* csgate - ear test for the 74HCT02 chip-select gate                  */
/* ------------------------------------------------------------------ */
/*
 * If U2 pin10 is wired straight to CSn instead of through the NOR gate, the
 * SID is selected during the ADDRESS phase too, so every write also writes
 * `addr` into the previously latched register. On an otherwise silent chip
 * that shows up as a burst of clicks and fizz.
 *
 * This command leaves the chip silent, then hammers it with address-phase-only
 * bus cycles (Path B with A0=0). A correctly gated module stays completely
 * silent. An ungated one crackles.
 */
static int cmd_csgate(void)
{
    if (connect_device() != 0) return 1;

    printf("silencing the chip...\n");
    sid_clear_all_regs(g_slot);
    sid_flush();
    sleep_ms(300);

    printf("\nphase 1: 400 address-phase-only cycles (A0=0, D=0x04)\n"
           "  a properly gated /CS keeps the SID deselected here -> SILENCE\n"
           "  if you hear clicks/fizz, /CS is not going through the 74HCT02\n");
    fflush(stdout);
    for (int i = 0; i < 400; i++) {
        spfm_write_path_b(g_slot, 0x00, 0x04);
        if ((i % 20) == 19) { spfm_flush(); sleep_ms(20); }
    }
    spfm_flush();
    sleep_ms(500);
    printf("  phase 1 done.\n");

    printf("\nphase 2: 400 real writes of 0x00 to register 0x00\n"
           "  this should also be silent (writing zeros to a zeroed register)\n");
    fflush(stdout);
    for (int i = 0; i < 400; i++) {
        sid_write(g_slot, 0x00, 0x00);
        if ((i % 20) == 19) { spfm_flush(); sleep_ms(20); }
    }
    sid_flush();
    sleep_ms(500);
    printf("  phase 2 done.\n");

    printf("\nverdict:\n"
           "  silent both phases      -> gate is working, look elsewhere\n"
           "  crackle in phase 1 only -> /CS is NOT gated, fix U2 pin10\n"
           "  crackle in both         -> writes do land, but are being lost /\n"
           "                             retriggered; try --repeat\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* cswalk - static bus states for measuring /CS with a meter           */
/* ------------------------------------------------------------------ */
/*
 * The ear test (csgate) tells you *whether* /CS is gated. This tells you
 * *where* the gate is broken, by parking the bus in each phase for three
 * seconds so a multimeter can be held on SID pin 6 and on U2.
 */
static int cmd_cswalk(void)
{
    if (connect_device() != 0) return 1;

    printf(
        "Static bus walk. Each step runs for ~3 s so you can measure with a\n"
        "multimeter. Keep the probe on SID pin 6 (/CS) unless told otherwise.\n\n");

    sid_clear_all_regs(g_slot);
    sid_flush();
    sleep_ms(500);

    printf("[1/4] idle, no bus activity\n");
    sleep_ms(3000);
    printf("      SID pin 6 should sit at ~5 V.\n"
           "      U2 pin 10 should sit at ~5 V.\n\n");

    printf("[2/4] ADDRESS phase only: A0=0, A1-A3=0, D=0x04, /WR pulses\n");
    fflush(stdout);
    for (int i = 0; i < 300; i++) {
        spfm_write_path_b(g_slot, 0x00, 0x04);
        if ((i % 10) == 9) { spfm_flush(); sleep_ms(100); }
    }
    spfm_flush();
    printf("      SID pin 6 MUST STAY HIGH (~5 V, pulses only if broken).\n"
           "      U2 pin 1 (LE) SHOULD BE HIGH (pulsing).\n"
           "      U2 pin 10 should be HIGH.\n"
           "      --> pin 6 pulsing low here  = /CS not gated (fault A)\n"
           "      --> U2 pin 1 low/undefined  = LE path broken (fault B)\n\n");

    printf("[3/4] DATA phase only: A0=1, D=0x00, /WR pulses\n");
    fflush(stdout);
    for (int i = 0; i < 300; i++) {
        spfm_write_path_b(g_slot, 0x01, 0x00);
        if ((i % 10) == 9) { spfm_flush(); sleep_ms(100); }
    }
    spfm_flush();
    printf("      SID pin 6 SHOULD PULSE LOW here (this is the real write).\n"
           "      U2 pin 1 (LE) SHOULD BE LOW.\n"
           "      --> pin 6 never goes low = /CS stuck high, chip never written\n\n");

    printf("[4/4] full two-phase write, what a real register write looks like\n");
    fflush(stdout);
    for (int i = 0; i < 300; i++) {
        spfm_write_two_phase(g_slot, 0x18, 0x0F);
        if ((i % 10) == 9) { spfm_flush(); sleep_ms(100); }
    }
    spfm_flush();

    printf(
        "\nhow to read the results:\n"
        "  A) pin 6 pulses low during step 2\n"
        "     -> SID pin 6 is wired to CSn directly, or U2 pin 10 is not\n"
        "        connected to SID pin 6. Re-check that one wire.\n"
        "  B) pin 6 stays high in step 2 but also stays high in step 3\n"
        "     -> /CS never asserts. U2 pin 10 may be floating or the wrong\n"
        "        gate is used (check pin 8/9 -> pin 4 -> pin 8/9 loop).\n"
        "  C) step 2 ok (high) and step 3 ok (pulses)\n"
        "     -> gating is fine, so the fault is elsewhere: PHI2 not running,\n"
        "        or +12 V missing. Measure SID pin 8 and pin 28 next.\n"
        "  D) if U2 is marked 74HC02 (not 74HCT02), replace it. At 3.3 V bus\n"
        "     levels an HC part may not see the A0/CSn highs at all.\n");

    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* slotsweep - find which slot the SID is really on                    */
/* ------------------------------------------------------------------ */
/*
 * If the firmware's slot numbering does not match the physical position of
 * the SID, every write goes to an empty slot: the bus toggles (so the audio
 * picks up crosstalk) but the SID is never selected, so there is never a tone.
 * Four slots, four different pitches, two seconds each.
 */
static int cmd_slotsweep(void)
{
    static const uint8_t notes[4] = { 48, 55, 60, 67 };  /* C3 G3 C4 G4 */
    static const char *names[4] = { "C3", "G3", "C4", "G4" };

    if (connect_device() != 0) return 1;

    printf("each slot gets a 2 second note at a different pitch.\n"
           "listen for which one actually plays.\n\n");

    for (int s = 0; s < 4; s++) {
        printf("slot %d -> %s ... ", s, names[s]);
        fflush(stdout);

        spfm_send_reset();
        sid_set_write_mode(SID_WRITE_TWO_PHASE);
        sid_set_write_delay(90);
        sid_set_write_repeat(8);   /* hedge against the PHI2 phase problem */
        sid_init((uint8_t)s, 15);
        sid_note_on((uint8_t)s, 0, notes[s], SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(2000);
        sid_note_off((uint8_t)s, 0);
        sid_flush();
        sleep_ms(300);
        printf("done\n");
    }

    sid_set_write_repeat(1);
    spfm_send_reset();
    printf("\nwhich slot number played %s?\n"
           "  none of them -> the SID is never selected: check PHI2 (pin 8)\n"
           "                  and +12V (pin 28) before anything else\n",
           "a tone");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* xtalk - the one test we keep. replays step 4 under different timing */
/* ------------------------------------------------------------------ */
/*
 * Every write below is the exact sequence step 4 used (init volume, then
 * triangle A4 with gate), only the address-phase -> data-phase gap changes.
 *
 *   burst   one Path A record, firmware emits both cycles back to back
 *   tight   two Path B records with no gap, in one USB transfer
 *   nopN    two Path B records separated by N hardware NOP ticks (0x80),
 *           which is the firmware's own delay instruction, not host sleep
 *
 * `nopN` is the interesting one: 0x80 in first-byte position makes the STM32
 * busy-loop, so the gap is measured in firmware cycles and is immune to USB
 * scheduling. N goes 1..8 so the sweep covers roughly 1 to 20 us.
 */
static int cmd_xtalk(void)
{
    struct step {
        const char *label;
        sid_write_mode_t mode;
        uint32_t gap;
        uint32_t delay_us;
    } steps[] = {
        { "burst        Path A, no host timing at all",  SID_WRITE_BURST,           0,    0 },
        { "tight        two records, zero gap",          SID_WRITE_TWO_PHASE,        0,    0 },
        { "nop1         1 hardware NOP tick",            SID_WRITE_TWO_PHASE_NOP,    1,    0 },
        { "nop2         2 hardware NOP ticks",           SID_WRITE_TWO_PHASE_NOP,    2,    0 },
        { "nop3         3 hardware NOP ticks",           SID_WRITE_TWO_PHASE_NOP,    3,    0 },
        { "nop4         4 hardware NOP ticks",           SID_WRITE_TWO_PHASE_NOP,    4,    0 },
        { "nop6         6 hardware NOP ticks",           SID_WRITE_TWO_PHASE_NOP,    6,    0 },
        { "nop8         8 hardware NOP ticks",           SID_WRITE_TWO_PHASE_NOP,    8,    0 },
        { "nop16        16 hardware NOP ticks",          SID_WRITE_TWO_PHASE_NOP,   16,    0 },
        { "nop32        32 hardware NOP ticks",          SID_WRITE_TWO_PHASE_NOP,   32,    0 },
        { "soft90       two records + 90 us host sleep", SID_WRITE_TWO_PHASE,        0,   90 },
        { "soft500      two records + 500 us host sleep",SID_WRITE_TWO_PHASE,        0,  500 },
        { "pa+soft90    Path A + 90 us host sleep",      SID_WRITE_PATH_A_SOFT,      0,   90 },
    };
    const int n = (int)(sizeof steps / sizeof steps[0]);

    if (connect_device() != 0) return 1;

    sid_set_write_repeat(1);

    printf("each step: reset, volume, triangle A4, gate on, hold 4 s.\n"
           "judge by ear which ones hold a steady tone.\n"
           "nopN is a firmware busy-loop (exact); softN is a host sleep.\n\n");

    for (int i = 0; i < n; i++) {
        sid_set_write_mode(steps[i].mode);
        sid_set_phase_gap(steps[i].gap);
        sid_set_write_delay(steps[i].delay_us);

        printf("[%d/%d] %s\n", i + 1, n, steps[i].label);
        fflush(stdout);

        spfm_send_reset();
        sid_init(g_slot, 15);
        sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(4000);

        sid_note_kill(g_slot, 0);
        sid_flush();
        sleep_ms(600);
        printf("        done\n");
    }

    sid_clear_all_regs(g_slot);
    sid_flush();
    printf("\nwhich step held a steady A4? the nopN ones are hardware timed.\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* adsr demo: one note per ADSR corner                                 */
/* ------------------------------------------------------------------ */
/*
 * Everything you hear so far is bus crosstalk, not the SID. A 6581 produces
 * no tone at all if any of these is wrong, and all of them are upstream of
 * the volume register, so no amount of register fiddling can fix it:
 *
 *   pin 5  /RES   must sit at ~5 V (if it is low, the chip is held in reset)
 *   pin 8  PHI2   must be a 1 MHz square (no clock = no oscillator, no tone)
 *   pin 25 Vcc    5 V
 *   pin 28 Vdd    12 V  (8580: 9 V)
 *   pin 6  /CS    ~5 V when idle, pulses low only during a data phase
 *
 * This command leaves the bus completely idle so a meter can be held on each
 * pin without the bus noise confusing the reading.
 */
static int cmd_pins(void)
{
    if (connect_device() != 0) return 1;

    printf("parking the bus idle for 20 s. measure now.\n\n");

    /* Leave the chip silent and stop writing. The last thing on the bus is a
     * 0x18=0 write, so nothing is driving audio. */
    sid_set_write_repeat(1);
    sid_clear_all_regs(g_slot);
    sid_flush();

    printf(
        "  pin  5  /RES   expect ~5.0 V      (low = chip held in reset)\n"
        "  pin  6  /CS    expect ~5.0 V      (static low = always selected)\n"
        "  pin  8  PHI2   expect 1.000 MHz   (scope; a dead can = no sound)\n"
        "  pin 25  Vcc    expect 5.0 V\n"
        "  pin 28  Vdd    expect 12.0 V      (8580: 9 V)\n"
        "  pin 27  A OUT  expect a steady DC, no waveform\n"
        "\n"
        "also worth checking while it idles:\n"
        "  U1 pin 20 (374 VCC)  5 V\n"
        "  U2 pin 14 (02 VCC)   5 V\n"
        "  U2 pin  7 (02 GND)   0 V\n"
        "  Y1 pin  8 (can out)  1 MHz  -- if the can has 4 pins it is\n"
        "                       pin 8 = out, pin 1 = enable/NC, pin 4 = GND,\n"
        "                       pin 14/8 = VCC; a wrong can pinout is the\n"
        "                       single most common cause of this symptom\n"
        "\n");

    for (int i = 0; i < 20; i++) {
        sleep_ms(1000);
        printf("  %2d s\n", i + 1);
        fflush(stdout);
    }

    printf("\nif pin 5, 25 and 28 are right but pin 8 is dead, fix the clock.\n"
           "if all six pins are right and there is still no tone, the writes\n"
           "are not landing: run 'cswalk' and watch pin 6 during step 3.\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* env - is the ADSR envelope actually being written?                  */
/* ------------------------------------------------------------------ */
/*
 * A note that sounds for an instant and then dies, with AD=0x00 (attack 2 ms,
 * decay 6 ms) and SR=0xF0 (sustain 15, release 0), means the sustain level is
 * reading back as 0: register 0x06 is not landing.
 *
 * Three phases, each clearly identifiable by ear:
 *   A) slow attack  -> if you hear a slow swell, 0x05 lands
 *   B) long sustain -> if the tone holds, 0x06 lands
 *   C) refresh      -> rewriting 0x06 during the note; if the tone comes back
 *                      to life, the register does land but something
 *                      overwrites it, which points at /CS gating
 */
static int cmd_env(void)
{
    if (connect_device() != 0) return 1;

    sid_init(g_slot, 15);
    sid_flush();

    /* ---- A: slow attack, proves register 0x05 is written ---- */
    printf("A) slow attack 8 s: AD=0xF0, SR=0xF0, note A4\n"
           "   expect a long slow swell over ~8 s, then a steady tone.\n"
           "   no swell at all -> 0x05 (attack/decay) is not landing.\n");
    fflush(stdout);
    sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0xF0, 0xF0);
    sid_flush();
    for (int i = 0; i < 100; i++) {          /* 10 s */
        sleep_ms(100);
        if ((i % 10) == 9) printf(".");
        fflush(stdout);
    }
    printf("\n");
    sid_note_kill(g_slot, 0);
    sid_flush();
    sleep_ms(800);

    /* ---- B: instant attack, full sustain ---- */
    printf("\nB) instant attack, sustain 15: AD=0x00, SR=0xF0, note A4\n"
           "   expect a tone that starts at once and holds for the full 10 s.\n"
           "   blip then silence -> 0x06 (sustain/release) is not landing.\n");
    fflush(stdout);
    sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0x00, 0xF0);
    sid_flush();
    for (int i = 0; i < 100; i++) {
        sleep_ms(100);
        if ((i % 10) == 9) printf(".");
        fflush(stdout);
    }
    printf("\n");
    sid_note_kill(g_slot, 0);
    sid_flush();
    sleep_ms(800);

    /* ---- C: keep refreshing the envelope while the note runs ---- */
    printf("\nC) same as B, but 0x06 and gate are rewritten every 200 ms\n"
           "   if THIS holds a steady tone while B did not, then the write\n"
           "   lands but gets clobbered -- suspect the /CS gate.\n");
    fflush(stdout);
    sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0x00, 0xF0);
    sid_flush();
    for (int i = 0; i < 50; i++) {           /* 10 s */
        sleep_ms(200);
        sid_write(g_slot, 0x06, 0xF0);       /* sustain 15, release 0 */
        sid_write(g_slot, 0x04, SID_CTL_TRI | SID_CTL_GATE);
        sid_flush();
        if ((i % 5) == 4) printf(".");
        fflush(stdout);
    }
    printf("\n");
    sid_note_kill(g_slot, 0);
    sid_flush();

    printf("\nreport which of A / B / C produced a lasting tone.\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* retry - same note, only the retry count changes                     */
/* ------------------------------------------------------------------ */
/*
 * Evidence that led here:
 *   - every two-phase gap from 0 to 32 hardware NOP ticks failed to sustain
 *   - but the env test's phase C, which rewrote 0x06 + gate every 200 ms,
 *     DID sustain it
 *
 * Same bus timing in both cases, only the number of attempts differs. So the
 * problem is that individual writes are being lost, not that the timing is
 * wrong: losing 0x06 leaves sustain = 0, the envelope decays and the tone
 * vanishes -- exactly the "one instant, then silence" symptom.
 *
 * Everything else is held constant (two-phase records, triangle A4, AD=0x00,
 * SR=0xF0, four seconds). The first row that sustains tells us how many
 * retries this hardware needs.
 */
static int cmd_retry(void)
{
    static const uint32_t repeats[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
    const int n = (int)(sizeof repeats / sizeof repeats[0]);

    if (connect_device() != 0) return 1;

    sid_set_write_mode(SID_WRITE_TWO_PHASE);
    sid_set_phase_gap(0);
    sid_set_write_delay(0);

    printf("triangle A4, AD=0x00, SR=0xF0, held 4 s per row.\n"
           "report the first retry count that holds the note.\n\n");

    for (int i = 0; i < n; i++) {
        sid_set_write_repeat(repeats[i]);

        printf("repeat=%-3lu  ", (unsigned long)repeats[i]);
        fflush(stdout);

        spfm_send_reset();
        sid_init(g_slot, 15);
        sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(4000);

        sid_note_kill(g_slot, 0);
        sid_flush();
        sleep_ms(600);
        printf("done\n");
    }

    sid_set_write_repeat(1);
    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* prelude - does leftover state from earlier commands matter?         */
/* ------------------------------------------------------------------ */
/*
 * The first run of the old xtalk held a tone once, a re-run did not. The only
 * difference is what the chip had been through before the note. Each variant
 * builds a different precondition, then plays the identical note.
 */
static int cmd_prelude(void)
{
    if (connect_device() != 0) return 1;

    sid_set_write_mode(SID_WRITE_TWO_PHASE);
    sid_set_write_delay(0);
    sid_set_write_repeat(8);      /* known to work sometimes, so only the
                                   * precondition varies */

    for (int v = 0; v < 7; v++) {
        printf("[%d/7] ", v + 1);
        fflush(stdout);

        switch (v) {
        case 0:
            printf("reset -> init -> note                       ");
            spfm_send_reset();
            sid_init(g_slot, 15);
            break;
        case 1:
            printf("init -> note (no reset)                     ");
            sid_init(g_slot, 15);
            break;
        case 2:
            printf("reset -> hammer -> init -> note             ");
            spfm_send_reset();
            for (int i = 0; i < 2000; i++) {
                spfm_write_path_b(g_slot, 0x00, 0x11);
                if ((i % 10) == 9) { spfm_flush(); sleep_ms(20); }
            }
            spfm_flush();
            sid_init(g_slot, 15);
            break;
        case 3:
            printf("reset -> init -> hammer -> note             ");
            spfm_send_reset();
            sid_init(g_slot, 15);
            for (int i = 0; i < 2000; i++) {
                spfm_write_path_b(g_slot, 0x00, 0x11);
                if ((i % 10) == 9) { spfm_flush(); sleep_ms(20); }
            }
            spfm_flush();
            break;
        case 4:
            printf("init x4 -> note                             ");
            for (int k = 0; k < 4; k++) sid_init(g_slot, 15);
            break;
        case 5:
            printf("note only, no init at all                   ");
            break;
        case 6:
            printf("reset -> pause 3s -> init -> note           ");
            spfm_send_reset();
            sleep_ms(3000);
            sid_init(g_slot, 15);
            break;
        }

        sid_note_on(g_slot, 0, 69, SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(4000);
        sid_note_kill(g_slot, 0);
        sid_flush();
        sleep_ms(600);
        printf("done\n");
    }

    sid_set_write_repeat(1);
    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* split - sweep address repeats against data repeats                  */
/* ------------------------------------------------------------------ */
/*
 * The two phases fail for different reasons, so this walks the grid and plays
 * one short note per cell. The address repeat only needs to be large enough
 * for the 74LS374 to latch (it reloads the same value every time, so extra
 * repeats are free); the data repeat is the one that has to cover enough PHI2
 * edges for the SID to sample.
 *
 * Notes climb with the grid index so a cell that tracks pitch is instantly
 * distinguishable from a cell that is stuck on one note.
 */
static int cmd_split(void)
{
    static const uint32_t addr_reps[] = { 1, 2, 4, 8 };
    static const uint32_t data_reps[] = { 1, 2, 4, 8, 16 };

    if (connect_device() != 0) return 1;

    sid_set_write_mode(SID_WRITE_SPLIT);
    sid_set_phase_gap(3);
    sid_set_write_delay(0);
    sid_set_write_repeat(1);      /* the counters inside split do the scaling */

    printf("split mode, gap=3 hardware ticks. each row: addr xA, data xD.\n"
           "notes walk upward so a row that tracks pitch is obvious.\n"
           "DIAGNOSTIC: run 'split' rather than declaring a winner early.\n\n");

    uint8_t note = 48;
    for (size_t a = 0; a < sizeof addr_reps / sizeof addr_reps[0]; a++) {
        for (size_t d = 0; d < sizeof data_reps / sizeof data_reps[0]; d++) {
            sid_set_addr_repeat(addr_reps[a]);
            sid_set_data_repeat(data_reps[d]);

            printf("addr x%-2lu data x%-2lu  note %3u ... ",
                   (unsigned long)addr_reps[a],
                   (unsigned long)data_reps[d], (unsigned)note);
            fflush(stdout);

            spfm_send_reset();
            sid_init(g_slot, 15);
            sid_note_on(g_slot, 0, note, SID_CTL_TRI, 0x00, 0xF0);
            sid_flush();
            sleep_ms(1200);
            sid_note_kill(g_slot, 0);
            sid_flush();
            sleep_ms(200);
            printf("done\n");

            note += 2;
            if (note > 84) note = 48;
        }
    }

    sid_set_write_mode(SID_WRITE_TWO_PHASE_NOP);
    sid_set_addr_repeat(4);
    sid_set_data_repeat(8);
    sid_set_write_repeat(12);
    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* norep - one write per USB transfer, sweep the gap, no retries       */
/* ------------------------------------------------------------------ */
/*
 * The point of this command is to get a stable pitch WITHOUT relying on
 * repeat. One register write = one FT_Write, exactly like the YM7129 driver.
 *
 * Why the gap matters at all: one byte at 1.5 Mbaud (8N1) is 6.667 us while
 * PHI2 is 1 us, so a record of 6+gap bytes lasts (6+gap) * 6.667 us. That is
 * an exact multiple of the PHI2 period when (6+gap) is a multiple of 3, i.e.
 * gap = 0, 3, 6, 9. With those gaps every write lands on the same PHI2
 * phase, so the whole run either works or fails as one. Gaps 1, 2, 4, 5, 7, 8
 * shift the data phase by 0.33 or 0.67 of a period, which is what lets a
 * single (non-repeated) write hit the sampling edge.
 */
static int cmd_norep(void)
{
    if (connect_device() != 0) return 1;

    sid_set_write_mode(SID_WRITE_TWO_PHASE_NOP);
    sid_set_write_delay(0);
    sid_set_write_repeat(1);          /* the whole point: no retries */

    printf("one write per USB transfer, repeat=1, gap swept 0..8.\n"
           "pitch rises with the gap so you can hear which one tracks it.\n"
           "gap 0/3/6 are expected to be phase-locked (all-or-nothing).\n\n");

    static const uint32_t gaps[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t note = 48;

    for (size_t i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        sid_set_phase_gap(gaps[i]);

        printf("gap=%-2lu note %3u ... ", (unsigned long)gaps[i], (unsigned)note);
        fflush(stdout);

        spfm_send_reset();
        sid_init(g_slot, 15);
        sid_note_on(g_slot, 0, note, SID_CTL_TRI, 0x00, 0xF0);
        sid_flush();
        sleep_ms(2500);
        sid_note_kill(g_slot, 0);
        sid_flush();
        sleep_ms(400);
        printf("done\n");

        note = (uint8_t)(note + 3);
        if (note > 81) note = 48;
    }

    sid_set_phase_gap(3);
    sid_set_write_repeat(12);
    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* adsr demo: one note per ADSR corner                                 */
/* ------------------------------------------------------------------ */
static int cmd_adsr(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    struct { uint8_t ad, sr; const char *name; } cases[] = {
        { 0x00, 0xF0, "fast attack, full sustain, instant release" },
        { 0x60, 0xF2, "midi.c default" },
        { 0xF0, 0x0F, "slow attack, no sustain, slow release" },
        { 0x09, 0xF9, "short blip" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        printf("%-45s AD=0x%02X SR=0x%02X\n", cases[i].name,
               cases[i].ad, cases[i].sr);
        sid_note_on(g_slot, 0, 60, SID_CTL_TRI, cases[i].ad, cases[i].sr);
        sid_flush();
        sleep_ms(1500);
        sid_note_off(g_slot, 0);
        sid_flush();
        sleep_ms(1500);
    }

    if (g_dry_run) {
        printf("\n--- captured byte stream ---\n");
        spfm_capture_dump(stdout);
    }
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* checkup - the five power-on checks from the wiring doc              */
/* ------------------------------------------------------------------ */
static void wait_enter(const char *prompt)
{
    char line[64];
    printf("\n>> %s\n   press Enter to continue (or 'q' to abort): ", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return;
    if (line[0] == 'q' || line[0] == 'Q') exit(0);
}

static int cmd_checkup(void)
{
    printf("SID power-on checkup, slot %u\n", (unsigned)g_slot);
    printf("This drives the bus and waits for you to measure. A multimeter on\n"
           "SID pin 27 (A OUT) and an oscilloscope are assumed.\n");

    wait_enter("Measure Y1 output: must be 1.000 MHz square. "
               "Measure SID pin 25 = 5 V, pin 28 = 12 V (8580: 9 V).");

    if (connect_device() != 0) return 1;

    printf("\n[1/4] clearing all 25 registers\n");
    sid_clear_all_regs(g_slot);
    sid_flush();
    printf("      pin 27 should now sit at a steady DC level, no waveform.\n");
    wait_enter("Confirm pin 27 is quiet DC. If it buzzes, /CS is probably "
               "wired straight to CSn instead of through the 74HCT02.");

    printf("\n[2/4] volume = 15 (0x18 = 0x0F)\n");
    sid_write(g_slot, SID_REG_MODE_VOL, 0x0F);
    sid_flush();
    wait_enter("Pin 27 should shift DC slightly. If nothing changes, check "
               "that the volume write actually reached 0x18 (A4 line from "
               "74LS374 pin 12 to SID pin 13).");

    printf("\n[3/4] triangle + gate on voice 1 (0x04 = 0x11)\n");
    sid_write(g_slot, SID_REG_ATTACK_DECAY, 0x00);
    sid_write(g_slot, SID_REG_SUSTAIN_REL, 0xF0);
    sid_write(g_slot, SID_REG_FREQ_LO, 0x00);
    sid_write(g_slot, SID_REG_FREQ_HI, 0x10);
    sid_write(g_slot, SID_REG_CONTROL, SID_CTL_TRI | SID_CTL_GATE);
    sid_flush();
    wait_enter("Scope on pin 27: a triangle wave near 1 kHz is expected.");

    printf("\n[4/4] gate off\n");
    sid_write(g_slot, SID_REG_CONTROL, SID_CTL_TRI);
    sid_flush();
    printf("      waveform should stop.\n");

    printf("\ncheckup done.\n");
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* selftest - verify the generated byte stream without hardware        */
/* ------------------------------------------------------------------ */
static int failures = 0;

static void expect(int cond, const char *what)
{
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* Default write mode is two-phase, so a register write is one 6-byte record:
 *   [slot, 0x80, addr, slot, 0x81, data]
 * These helpers index into the capture by write number and by field. */
#define CAP_REC 6
static size_t cap_writes(void) { return spfm_capture_size() / CAP_REC; }
static uint8_t cap_reg(size_t i)  { return spfm_capture_data()[i * CAP_REC + 2]; }
static uint8_t cap_data(size_t i) { return spfm_capture_data()[i * CAP_REC + 5]; }

static int cmd_selftest(void)
{
    printf("byte-stream selftest (no hardware)\n");
    spfm_dry_run_enable(true);
    /* force the canonical one-record-per-write shape so the byte counts in
     * the assertions below are exact */
    sid_set_write_mode(SID_WRITE_TWO_PHASE);
    sid_set_phase_gap(0);
    sid_set_write_repeat(1);
    sid_set_write_delay(0);

    /* 1. default mode emits the YM7129 record shape. */
    spfm_capture_clear();
    sid_write(3, 0x18, 0x0F);
    {
        const uint8_t *b = spfm_capture_data();
        expect(spfm_capture_size() == 6, "one write is 6 bytes");
        expect(b[0] == 0x03, "addr phase: slot 3");
        expect(b[1] == 0x80, "addr phase: A0..A3 = 0");
        expect(b[2] == 0x18, "addr phase carries register 0x18");
        expect(b[3] == 0x03, "data phase: slot 3");
        expect(b[4] == 0x81, "data phase: A0 = 1");
        expect(b[5] == 0x0F, "data phase carries data 0x0F");
    }

    /* 2. address mask: registers above 0x1F never leak into bit7. */
    spfm_capture_clear();
    sid_write(3, 0x38, 0xAA);
    expect(cap_reg(0) == 0x18, "register masked to &0x1F");

    /* 3. init emits 25 clears + volume + 15 voice writes + 3 filter writes. */
    spfm_capture_clear();
    sid_init(3, 15);
    {
        size_t n = cap_writes();
        /* 25 clears, 1 volume, 3*(AD,SR,PWlo,PWhi,CTL)=15, filter 3 */
        expect(n == 25 + 1 + 15 + 3, "init write count");
        printf("       init produced %lu writes (expected 44)\n",
               (unsigned long)n);
    }

    /* 4. init must write the volume register with a non-zero low nibble. */
    {
        size_t n = cap_writes();
        int found = 0;
        for (size_t i = 0; i < n; i++) {
            if (cap_reg(i) == 0x18 && (cap_data(i) & 0x0F) == 15) found = 1;
        }
        expect(found, "init writes volume 15 to 0x18");
    }

    /* 5. note_on ordering: gate must be in the last control write. */
    spfm_capture_clear();
    sid_note_on(3, 0, 60, SID_CTL_TRI, 0x00, 0xF0);
    {
        expect(cap_writes() == 5, "note_on is 5 writes");
        expect(cap_reg(0) == 0x00, "write 0 = freq lo (0x00)");
        expect(cap_reg(1) == 0x01, "write 1 = freq hi (0x01)");
        expect(cap_reg(2) == 0x05, "write 2 = AD    (0x05)");
        expect(cap_reg(3) == 0x06, "write 3 = SR    (0x06)");
        expect(cap_reg(4) == 0x04, "write 4 = ctl   (0x04)");
        expect(cap_data(4) == (SID_CTL_TRI | SID_CTL_GATE),
               "control byte carries waveform | gate");
        uint16_t want = sid_freq_from_note(60);
        uint16_t got  = (uint16_t)(cap_data(0) | (cap_data(1) << 8));
        expect(got == want, "freq split lo/hi reconstructs table value");
        printf("       note 60 -> %u\n", (unsigned)want);
    }

    /* 6. voice 2 and 3 land at 0x07 and 0x0E. */
    spfm_capture_clear();
    sid_note_on(3, 1, 64, SID_CTL_SAW, 0, 0);
    expect(cap_reg(0) == 0x07, "voice 2 base = 0x07");
    spfm_capture_clear();
    sid_note_on(3, 2, 67, SID_CTL_SAW, 0, 0);
    expect(cap_reg(0) == 0x0E, "voice 3 base = 0x0E");

    /* 7. note_off keeps the waveform and only drops gate. */
    spfm_capture_clear();
    sid_note_off(3, 0);
    {
        expect(cap_writes() == 1, "note_off is one write");
        expect(cap_reg(0) == 0x04, "note_off writes control register");
        expect((cap_data(0) & SID_CTL_GATE) == 0, "note_off clears gate");
        expect((cap_data(0) & SID_CTL_WAVE_MASK) == SID_CTL_TRI,
               "note_off keeps the waveform");
    }

    /* 8. kill clears the waveform too. */
    spfm_capture_clear();
    sid_note_kill(3, 0);
    expect(cap_data(0) == 0x00, "note_kill clears control entirely");

    /* 9. PWM high nibble never exceeds 0x0F. */
    spfm_capture_clear();
    sid_set_pulse(3, 0, 0xFFFF);
    {
        expect(cap_data(0) == 0xFF, "PW lo = 0xFF");
        expect(cap_data(1) == 0x0F, "PW hi = 0x0F (4 bits only)");
        expect(cap_reg(1) == 0x03, "PW hi register = 0x03");
    }

    /* 10. filter registers: 11-bit cutoff split 3 + 8. */
    spfm_capture_clear();
    sid_set_filter_cutoff(3, 0x7FF);
    {
        expect(cap_reg(0) == 0x15 && cap_data(0) == 0x07, "cutoff lo 3 bits");
        expect(cap_reg(1) == 0x16 && cap_data(1) == 0xFF, "cutoff hi 8 bits");
    }

    /* 11. freq table is monotonic above note 12 and matches the formula. */
    {
        int mono = 1;
        for (int n = 13; n < 128; n++)
            if (sid_freq_table[n] < sid_freq_table[n - 1]) mono = 0;
        expect(mono, "freq table monotonic");
        expect(sid_freq_from_note(69) == 7382, "A4 = 7382 at 1 MHz");
        expect(sid_freq_from_note(60) == 4389, "C4 = 4389 at 1 MHz");
        expect(sid_freq_from_note(45) == 1845, "A2 = 1845 at 1 MHz");
    }

    /* 12. every record in a full sequence is well formed. */
    spfm_capture_clear();
    sid_init(3, 15);
    sid_note_on(3, 0, 60, SID_CTL_PULSE, REF_AD, REF_SR);
    {
        const uint8_t *b = spfm_capture_data();
        size_t n = spfm_capture_size();
        int ok = (n % CAP_REC == 0);
        for (size_t i = 0; ok && i + CAP_REC <= n; i += CAP_REC) {
            if (b[i] != 3 || b[i + 1] != 0x80) ok = 0;
            if (b[i + 3] != 3 || b[i + 4] != 0x81) ok = 0;
            if ((b[i + 2] & 0xE0) != 0) ok = 0;
        }
        expect(ok, "every record is slot3/A0=0/reg + slot3/A0=1/data");
    }

    /* 13. path A mode still produces the 4-byte command. */
    sid_set_write_mode(SID_WRITE_BURST);
    spfm_capture_clear();
    sid_write(3, 0x18, 0x0F);
    {
        const uint8_t *b = spfm_capture_data();
        expect(spfm_capture_size() == 4, "path-a write is 4 bytes");
        expect(b[0] == 0x03 && b[1] == 0x00 && b[2] == 0x18 && b[3] == 0x0F,
               "path-a bytes are [slot,0x00,reg,data]");
    }

    /* 14. NOP variant inserts the gap bytes instead of a host sleep. */
    sid_set_write_mode(SID_WRITE_TWO_PHASE_NOP);
    sid_set_phase_gap(2);
    spfm_capture_clear();
    sid_write(3, 0x18, 0x0F);
    {
        const uint8_t *b = spfm_capture_data();
        expect(spfm_capture_size() == 3 + 2 + 3, "nop variant is 3 + 2 NOP + 3");
        expect(b[3] == 0x80 && b[4] == 0x80, "gap bytes are NOP ticks");
        expect(b[5] == 0x03 && b[6] == 0x81 && b[7] == 0x0F,
               "data phase follows the gap");
    }

    sid_set_write_mode(SID_WRITE_TWO_PHASE);
    sid_set_phase_gap(0);

    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* interactive                                                         */
/* ------------------------------------------------------------------ */
static void interactive_usage(void)
{
    printf(
        "commands:\n"
        "  r <reg> <data>       write register (hex ok, e.g. r 18 0f)\n"
        "  n <voice> <note>     note on with current waveform\n"
        "  o <voice>            note off (release)\n"
        "  k <voice>            note kill (immediate)\n"
        "  w <wave>             waveform for the next n: tri|saw|pulse|noise\n"
        "  W <voice> <wave>     change a sounding voice's waveform\n"
        "  f <voice> <freq>     frequency register, decimal or 0x\n"
        "  p <voice> <pw>       pulse width 0..4095\n"
        "  v <vol>              volume 0..15\n"
        "  i                    re-init\n"
        "  q                    quit\n");
}

static uint8_t parse_wave(const char *w, uint8_t dflt)
{
    if (!strcmp(w, "tri"))   return SID_CTL_TRI;
    if (!strcmp(w, "saw"))   return SID_CTL_SAW;
    if (!strcmp(w, "pulse")) return SID_CTL_PULSE;
    if (!strcmp(w, "noise")) return SID_CTL_NOISE;
    return dflt;
}

/* Interactive numbers are typed by hand, so accept 0x1F, 1f, 18 and 24 alike:
 * an explicit 0x means hex, a bare token containing a-f is hex, otherwise
 * decimal. "10" stays decimal so volume 10 does not become 16. */
static long parse_num(const char *s)
{
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return strtol(s + 2, NULL, 16);
    for (const char *p = s; *p; p++) {
        if (isalpha((unsigned char)*p)) return strtol(s, NULL, 16);
    }
    return strtol(s, NULL, 10);
}

static int cmd_interactive(void)
{
    if (connect_device() != 0) return 1;
    sid_init(g_slot, 15);

    printf("interactive mode, slot %u\n", (unsigned)g_slot);
    interactive_usage();

    uint8_t wave = SID_CTL_TRI;
    char line[256];

    for (;;) {
        printf("sid> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        char cmd = *p++;
        char t1[32] = {0}, t2[32] = {0};
        int nargs = sscanf(p, "%31s %31s", t1, t2);
        long a = parse_num(t1), bb = parse_num(t2);

        switch (cmd) {
        case 'q':
            goto done;
        case 'r':
            if (nargs >= 2) {
                sid_write(g_slot, (uint8_t)(a & 0x1F), (uint8_t)(bb & 0xFF));
                printf("  0x%02lX <- 0x%02lX\n", a & 0x1F, bb & 0xFF);
            } else {
                printf("  usage: r <reg> <data>\n");
            }
            break;
        case 'n':
            if (nargs >= 2) {
                sid_note_on(g_slot, (uint8_t)a, (uint8_t)bb, wave, REF_AD, REF_SR);
                printf("  voice %ld note %ld wave 0x%02X\n", a, bb, wave);
            } else {
                printf("  usage: n <voice> <note>\n");
            }
            break;
        case 'o':
            if (nargs >= 1) { sid_note_off(g_slot, (uint8_t)a); printf("  off %ld\n", a); }
            else printf("  usage: o <voice>\n");
            break;
        case 'k':
            if (nargs >= 1) { sid_note_kill(g_slot, (uint8_t)a); printf("  kill %ld\n", a); }
            else printf("  usage: k <voice>\n");
            break;
        case 'w':
        case 'W': {
            /* two forms: "w tri" or "w 0 tri" */
            char w1[32] = {0}, w2[32] = {0};
            int nw = sscanf(p, "%31s %31s", w1, w2);
            if (nw == 1) {
                wave = parse_wave(w1, wave);
                printf("  next note wave 0x%02X\n", wave);
            } else if (nw == 2) {
                long v = parse_num(w1);
                wave = parse_wave(w2, wave);
                sid_set_wave(g_slot, (uint8_t)v, wave);
                printf("  voice %ld wave 0x%02X\n", v, wave);
            } else {
                printf("  usage: w <wave> | w <voice> <wave>\n");
            }
            break;
        }
        case 'f':
            if (nargs >= 2) {
                sid_set_freq(g_slot, (uint8_t)a, (uint16_t)bb);
                printf("  voice %ld freq %ld\n", a, bb);
            } else if (nargs == 1) {
                sid_set_freq(g_slot, 0, (uint16_t)a);
                printf("  voice 0 freq %ld\n", a);
            } else {
                printf("  usage: f <voice> <freq> | f <freq>\n");
            }
            break;
        case 'p':
            if (nargs >= 2) {
                sid_set_pulse(g_slot, (uint8_t)a, (uint16_t)bb);
                printf("  voice %ld pw %ld\n", a, bb);
            } else if (nargs == 1) {
                sid_set_pulse(g_slot, 0, (uint16_t)a);
                printf("  voice 0 pw %ld\n", a);
            } else {
                printf("  usage: p <voice> <pw> | p <pw>\n");
            }
            break;
        case 'v':
            if (nargs >= 1) { sid_set_volume(g_slot, (uint8_t)a); printf("  vol %ld\n", a); }
            else printf("  usage: v <0..15>\n");
            break;
        case 'i':
            sid_init(g_slot, 15);
            printf("  re-init done\n");
            break;
        case '?':
        case 'h':
            interactive_usage();
            break;
        default:
            printf("  ? unknown command\n");
            break;
        }
        sid_flush();
    }
done:
    sid_clear_all_regs(g_slot);
    sid_flush();
    disconnect();
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* Accept the common write-timing options after the command name too, so
 * `scale --step 500 --repeat 4` works the same as `--repeat 4 scale ...`.
 * Returns the number of remaining (non-option) arguments. */
static int strip_common_opts(char **argv, int argc)
{
    int w = 0;
    for (int r = 0; r < argc; ) {
        int consumed = 0;
        if (!strcmp(argv[r], "--addr-rep") && r + 1 < argc) {
            sid_set_addr_repeat((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--data-rep") && r + 1 < argc) {
            sid_set_data_repeat((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--gap-rand") && r + 1 < argc) {
            sid_set_gap_random((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--repeat") && r + 1 < argc) {
            sid_set_write_repeat((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--delay") && r + 1 < argc) {
            sid_set_write_delay((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--gap") && r + 1 < argc) {
            sid_set_phase_gap((uint32_t)strtoul(argv[r + 1], NULL, 0));
            consumed = 2;
        } else if (!strcmp(argv[r], "--write-mode") && r + 1 < argc) {
            const char *m = argv[r + 1];
            if (!strcmp(m, "two-phase"))            sid_set_write_mode(SID_WRITE_TWO_PHASE);
            else if (!strcmp(m, "two-phase-nop"))   sid_set_write_mode(SID_WRITE_TWO_PHASE_NOP);
            else if (!strcmp(m, "split"))           sid_set_write_mode(SID_WRITE_SPLIT);
            else if (!strcmp(m, "burst") ||
                     !strcmp(m, "path-a"))          sid_set_write_mode(SID_WRITE_BURST);
            else if (!strcmp(m, "path-a-soft"))     sid_set_write_mode(SID_WRITE_PATH_A_SOFT);
            sidplay_set_proto_explicit();
            consumed = 2;
        } else if (!strcmp(argv[r], "--proto") && r + 1 < argc) {
            if (sidplay_set_proto(argv[r + 1]) != 0)
                fprintf(stderr, "--proto must be 1step or 2step\n");
            consumed = 2;
        } else if (!strcmp(argv[r], "--timer") && r + 1 < argc) {
            /* Leave an invalid value in place so the subcommand reports it. */
            if (spfm_timer_set(argv[r + 1]) == 0) consumed = 2;
        } else if (!strcmp(argv[r], "--single")) {
            sid_set_write_repeat(1);
            consumed = 1;
        } else {
            argv[w++] = argv[r];
            consumed = 1;
        }
        r += consumed;
    }
    return w;
}

static void usage(void)
{
    printf(
        "sidtest - SPFM MOS 6581/8580 smoke test\n"
        "\n"
        "usage: sidtest [options] <command> [args]\n"
        "\n"
        "options:\n"
        "  --slot N        SPFM slot (default 3)\n"
        "  --device N      FTDI device index (default 0)\n"
        "  --dry-run       no hardware, decode the byte stream\n"
        "  --verbose       print every register write\n"
        "  --write-mode M  two-phase (default) | two-phase-nop | burst | "
        "path-a-soft | split\n"
        "  --proto P       1step | 2step: the write protocol used for playback\n"
        "                  (2step = two Path B commands, 1step = one Path A)\n"
        "  --timer T       hprec | hybrid | mmtimer | vgmplay: wait primitive\n"
        "  --flush G       register | command: USB flush granularity\n"
        "  --delay US      per-write host sleep (default 0)\n"
        "  --repeat N      send each write N times (default 1)\n"
        "  --gap N         NOP ticks between phases (default 0)\n"
        "  --addr-rep N    address-phase repeats in split mode (default 4)\n"
        "  --data-rep N    data-phase repeats in split mode (default 8)\n"
        "  --gap-rand N    add 0..N random hardware ticks to the gap "
        "(decorrelates PHI2 phase)\n"
        "  --single        shorthand for --repeat 1\n"
        "\n"
        "  --repeat/--delay/--gap/--write-mode are also accepted AFTER the\n"
        "  command name, e.g. `sidtest scale --step 500 --repeat 4`.\n"
        "\n"
        "commands:\n"
        "  list              enumerate FTDI devices\n"
        "  init [volume]     reset + initialise the chip\n"
        "  reg <r> <d> ...   raw register writes\n"
        "  scale             C major scale on voice 1\n"
        "  chord             C / Am / F / G / C, three voices\n"
        "  music             three-voice tune: pulse melody + tri bass + noise drums\n"
        "                    --song twinkle|ode|birthday|bach   --bpm N   --loops N   --list\n"
        "\n"
        "  .sid files (PSID/RSID) - run the 6502 once, replay the register stream:\n"
        "  sidinfo <f.sid>       header only (title, clock, subsong count)\n"
        "  sidextract <f.sid> <out.sidreg> [--seconds N] [--subsong N]\n"
        "                        render the stream, no hardware needed\n"
        "  sidplay <f.sid|.sidvgm|.sidreg> [--seconds N] [--subsong N] [--speed X]\n"
        "                        play one file\n"
        "  sidlist [dir=sid]     playlist, keys: n next, b back, , / . subsong, q quit\n"
        "                        --from N  --seconds N  --no-cache  --cache-dir DIR\n"
        "\n"
        "  USB transfer pacing (sidplay / sidlist):\n"
        "  --flush-gap MS   batch at least MS of stream into one USB transfer.\n"
        "                   Default = the old rule (flush whenever the next event\n"
        "                   is >0.09 ms away). On a digi tune that is ~6000\n"
        "                   transfers/s, 6x what FT232R schedules per USB frame.\n"
        "                   Try 1-5 for digi tunes; 0 = one transfer per frame.\n"
        "                   The run ends with a 'link: N transfers' line.\n"
        "  --pad MS         reverse of --flush-gap: fill gaps up to MS with NOP\n"
        "                   bytes so the 1.5 Mbaud UART clock carries the timing\n"
        "                   instead of host sleeps. Implies --dedup-vol.\n"
        "                   Try 0.3 for digi tunes; watch the wire duty in the link line.\n"
        "  --pad-duty PCT   cap the filler at PCT of the wire (default 75)\n"
        "  --pad-lead MS    playback latency / underrun margin (default 25)\n"
        "  --pad-chunk KB   bytes queued before a forced flush (default 2)\n"
        "  --pad-burst MS   leaky-bucket ceiling for the filler (default 200)\n"
        "  --pad-scope S    all|digi: pad every gap, or only gaps touching $D418\n"
        "  --pad-min MS     never pad a gap shorter than this (default 0)\n"
        "  --pad-partial    allow half-filled gaps (default all-or-nothing)\n"
        "  --pad-lag MS     stop padding when queued wire runs this far ahead\n"
        "  --pad-window MS  sliding window for the local filler cap (0 = off)\n"
        "  --pad-local PCT  max line use inside that window (default 100)\n"
        "  --pad-rate HZ    $D418 writes/s that count as a drum passage (def 200)\n"
        "                   --pad engages only inside such passages; every other\n"
        "                   part of the tune keeps the normal host-deadline path,\n"
        "                   so PSID tunes and digi intros keep their own tempo.\n"
        "  --pad-hold MS    keep padding this long after the drum stops (def 60)\n"
        "  --pad-always     ignore the gate and pad everywhere (old behaviour;\n"
        "                   only for A/B comparison or a constant-digi tune)\n"
        "  --digi HZ        resample ONLY the $D418 drum onto a 1/HZ grid.\n"
        "                   Melody/gate/ADSR untouched; 0 = off (default).\n"
        "                   8000 near-lossless, 4000/2000 grittier but cheaper.\n"
        "  --dedup-vol      drop $D418 writes that repeat the latched value\n"
        "  noise             noise generator frequency sweep\n"
        "  pwm               pulse width sweep\n"
        "  filter            filter cutoff sweep\n"
        "  adsr              four ADSR presets\n"
        "  tone [--note N] [--secs S] [--wave W]\n"
        "                    hold one note, judge by ear\n"
        "  probe             try every write-mode/delay/repeat combo, one per pitch\n"
        "  csgate            ear test for the 74HCT02 chip-select gate\n"
        "  cswalk            static bus states, measure /CS with a meter\n"
        "  slotsweep         try all four slots, one pitch each\n"
        "  xtalk             separate bus crosstalk from real sound\n"
        "  pins              park the bus, measure the SID supply/clock pins\n"
        "  env               is the ADSR envelope register landing?\n"
        "  retry             sweep the retry count, find what sustains\n"
        "  prelude           does leftover chip state change the result?\n"
        "  split             sweep address-repeat x data-repeat independently\n"
        "  norep             one write per transfer, gap swept 0..8, no retries\n"
        "  checkup           guided power-on checks (needs meters)\n"
        "  selftest          verify the byte stream, no hardware\n"
        "  interactive       type register writes by hand\n");
}

int main(int argc, char **argv)
{
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--slot") && i + 1 < argc) {
            g_slot = (uint8_t)atoi(argv[++i]);
            if (g_slot > SPFM_MAX_SLOT) {
                fprintf(stderr, "slot must be 0..%d\n", SPFM_MAX_SLOT);
                return 1;
            }
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            g_device = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dry-run")) {
            g_dry_run = 1;
        } else if (!strcmp(argv[i], "--write-mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "two-phase")) {
                sid_set_write_mode(SID_WRITE_TWO_PHASE);
            } else if (!strcmp(m, "two-phase-nop")) {
                sid_set_write_mode(SID_WRITE_TWO_PHASE_NOP);
            } else if (!strcmp(m, "burst") || !strcmp(m, "path-a")) {
                sid_set_write_mode(SID_WRITE_BURST);
            } else if (!strcmp(m, "path-a-soft")) {
                sid_set_write_mode(SID_WRITE_PATH_A_SOFT);
            } else if (!strcmp(m, "split")) {
                sid_set_write_mode(SID_WRITE_SPLIT);
            } else {
                fprintf(stderr, "--write-mode must be "
                        "burst|path-a-soft|two-phase|two-phase-nop|split\n");
                return 1;
            }
            sidplay_set_proto_explicit();
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            sid_set_write_delay((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            sid_set_write_repeat((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--addr-rep") && i + 1 < argc) {
            sid_set_addr_repeat((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--data-rep") && i + 1 < argc) {
            sid_set_data_repeat((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--gap") && i + 1 < argc) {
            sid_set_phase_gap((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--gap-rand") && i + 1 < argc) {
            sid_set_gap_random((uint32_t)strtoul(argv[++i], NULL, 0));
        } else if (!strcmp(argv[i], "--proto") && i + 1 < argc) {
            if (sidplay_set_proto(argv[++i]) != 0) {
                fprintf(stderr, "--proto must be 1step or 2step\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--timer") && i + 1 < argc) {
            if (spfm_timer_set(argv[i + 1]) != 0) {
                fprintf(stderr, "--timer must be hprec|hybrid|mmtimer|vgmplay\n");
                return 1;
            }
            i++;
        } else if (!strcmp(argv[i], "--verbose")) {
            g_verbose = 1;
            sid_set_write_hook(hook_print, NULL);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
        i++;
    }

    (void)g_verbose;

    if (i >= argc) {
        usage();
        return 1;
    }

    const char *cmd = argv[i++];
    char **rest = argv + i;
    int nrest = strip_common_opts(rest, argc - i);

    /* .sid playback shares the write-timing options above, so build one
     * options struct and hand it over. */
    sidplay_opts_t po;
    po.slot = g_slot;
    po.device = g_device;
    po.dry_run = g_dry_run;
    po.write_mode = 0;      /* already applied by strip_common_opts / main */

    if (!strcmp(cmd, "sidplay"))     return sidplay_do_play(nrest, rest, &po);
    if (!strcmp(cmd, "sidextract"))  return sidplay_do_extract(nrest, rest, &po);
    if (!strcmp(cmd, "sidinfo"))     return sidplay_do_info(nrest, rest, &po);
    if (!strcmp(cmd, "sidlist"))     return sidplay_do_list(nrest, rest, &po);

    if (!strcmp(cmd, "list"))        return cmd_list();
    if (!strcmp(cmd, "init"))        return cmd_init(nrest, rest);
    if (!strcmp(cmd, "reg"))         return cmd_reg(nrest, rest);
    if (!strcmp(cmd, "scale"))       return cmd_scale(nrest, rest);
    if (!strcmp(cmd, "chord"))       return cmd_chord(nrest, rest);
    if (!strcmp(cmd, "music"))       return cmd_music(nrest, rest);
    if (!strcmp(cmd, "noise"))       return cmd_noise(nrest, rest);
    if (!strcmp(cmd, "pwm"))         return cmd_pwm(nrest, rest);
    if (!strcmp(cmd, "filter"))      return cmd_filter(nrest, rest);
    if (!strcmp(cmd, "adsr"))        return cmd_adsr(nrest, rest);
    if (!strcmp(cmd, "tone"))        return cmd_tone(nrest, rest);
    if (!strcmp(cmd, "probe"))       return cmd_probe();
    if (!strcmp(cmd, "csgate"))      return cmd_csgate();
    if (!strcmp(cmd, "cswalk"))      return cmd_cswalk();
    if (!strcmp(cmd, "slotsweep"))   return cmd_slotsweep();
    if (!strcmp(cmd, "xtalk"))       return cmd_xtalk();
    if (!strcmp(cmd, "pins"))        return cmd_pins();
    if (!strcmp(cmd, "env"))         return cmd_env();
    if (!strcmp(cmd, "retry"))       return cmd_retry();
    if (!strcmp(cmd, "prelude"))     return cmd_prelude();
    if (!strcmp(cmd, "split"))       return cmd_split();
    if (!strcmp(cmd, "norep"))       return cmd_norep();
    if (!strcmp(cmd, "checkup"))     return cmd_checkup();
    if (!strcmp(cmd, "selftest"))    return cmd_selftest();
    if (!strcmp(cmd, "interactive")) return cmd_interactive();

    fprintf(stderr, "unknown command '%s'\n", cmd);
    usage();
    return 1;
}
