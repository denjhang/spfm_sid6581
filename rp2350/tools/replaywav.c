/* replaywav.c - render OUR event list through reSIDfp and write a WAV.
 *
 * The point is to compare our timeline with the reference player's audio
 * sample for sample: our t_ms values are fed to the same reSIDfp core that
 * build/lsfpref.exe uses, so any difference in the waveform is a difference in
 * *timing*, not in the chip model.
 *
 * Our t=0 is the first play burst (find_play_origin), which is somewhere inside
 * the reference render, so the two files are offset by a constant. That offset
 * is expected; a *changing* offset is the bug.
 *
 * usage: replaywav <file.sid> <out.wav> [seconds] [subsong]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

static void wav_header(FILE *f, unsigned rate, unsigned frames)
{
    unsigned char h[44];
    memset(h, 0, 44);
    unsigned data = frames * 2;
    memcpy(h, "RIFF", 4);
    unsigned riff = 36 + data;
    h[4] = (unsigned char)riff; h[5] = (unsigned char)(riff >> 8);
    h[6] = (unsigned char)(riff >> 16); h[7] = (unsigned char)(riff >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    h[24] = (unsigned char)rate; h[25] = (unsigned char)(rate >> 8);
    h[26] = (unsigned char)(rate >> 16); h[27] = (unsigned char)(rate >> 24);
    unsigned br = rate * 2;
    h[28] = (unsigned char)br; h[29] = (unsigned char)(br >> 8);
    memcpy(h + 32, "\x02\x00\x10\x00", 4);
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)data; h[41] = (unsigned char)(data >> 8);
    h[42] = (unsigned char)(data >> 16); h[43] = (unsigned char)(data >> 24);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: replaywav <file.sid> <out.wav> [seconds] [subsong]\n");
        return 1;
    }
    double seconds = (argc > 3) ? atof(argv[3]) : 4.0;
    int subsong = (argc > 4) ? atoi(argv[4]) : -1;

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    char err[200];
    if (sid_lsfp_extract(argv[1], subsong, seconds, &ev, &n, &info, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    fprintf(stderr, "events=%zu clock=%.0f %s\n", n, info.clock_hz,
            info.pal ? "PAL" : "NTSC");

    const double rate = 44100.0;
    sid_lsfp_render_t *r = sid_lsfp_render_new(info.clock_hz, 0, rate);
    if (!r) { fprintf(stderr, "render alloc failed\n"); return 1; }

    const unsigned want = (unsigned)(seconds * rate);
    short *out = (short *)calloc(want + 8192, sizeof *out);
    if (!out) return 1;

    unsigned done = 0;
    size_t i = 0;
    while (done < want) {
        /* How far to render before the next write, from our own t_ms. */
        double next_ms = (i < n) ? ev[i].t_ms : 1e18;
        unsigned upto = (unsigned)(next_ms / 1000.0 * rate + 0.5);
        if (upto > want) upto = want;
        if (upto < done) upto = done;      /* same-timestamp writes */
        if (upto > done) {
            sid_lsfp_render_render(r, out + done, upto - done);
            done = upto;
            if (i >= n) break;
        }
        while (i < n && (unsigned)(ev[i].t_ms / 1000.0 * rate + 0.5) <= done) {
            sid_lsfp_render_write(r, ev[i].reg, ev[i].val);
            i++;
        }
        if (i >= n && done >= want) break;
        if (i >= n) {                        /* tail after the last write */
            sid_lsfp_render_render(r, out + done, want - done);
            done = want;
            break;
        }
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    wav_header(f, (unsigned)rate, done);
    fwrite(out, 2, done, f);
    fclose(f);

    printf("wrote %s  %u frames (%.2f s)\n", argv[2], done, done / rate);
    sid_lsfp_render_free(r);
    free(out);
    sid_song_free(ev);
    return 0;
}
