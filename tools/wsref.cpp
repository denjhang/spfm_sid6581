/* Reference harness: render a .sid exactly the way the upstream WebSid
 * Raspberry Pi driver does (loadSidFile + playTune + Core::runOneFrame),
 * writing mono WAV. Used only to answer "is our renderer wrong or is the
 * stream wrong". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

extern "C" {
#include "core.h"
#include "system.h"
#include "vic.h"
#include "memory.h"
}
#include "sid.h"
#include "loaders.h"

extern "C" {
uint32_t loadSidFile(uint32_t is_mus, void *in_buffer, uint32_t in_buf_size,
                     uint32_t sample_rate, char *filename, void *basic_ROM,
                     void *char_ROM, void *kernal_ROM);
uint32_t playTune(uint32_t selected_track, uint32_t trace_sid);
}

static void wav_header(FILE *f, unsigned rate, unsigned frames)
{
    unsigned char h[44]; memset(h, 0, 44);
    unsigned data = frames * 2;
    memcpy(h, "RIFF", 4); h[4] = (unsigned char)((36+data)&0xFF);
    h[5] = (unsigned char)(((36+data)>>8)&0xFF);
    h[6] = (unsigned char)(((36+data)>>16)&0xFF);
    h[7] = (unsigned char)(((36+data)>>24)&0xFF);
    memcpy(h+8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    h[24] = (unsigned char)(rate&0xFF); h[25] = (unsigned char)((rate>>8)&0xFF);
    h[26] = (unsigned char)((rate>>16)&0xFF); h[27] = (unsigned char)((rate>>24)&0xFF);
    unsigned br = rate*2;
    h[28] = (unsigned char)(br&0xFF); h[29] = (unsigned char)((br>>8)&0xFF);
    h[30] = (unsigned char)((br>>16)&0xFF); h[31] = (unsigned char)((br>>24)&0xFF);
    h[32] = 2; h[34] = 16;
    memcpy(h+36, "data", 4);
    h[40] = (unsigned char)(data&0xFF); h[41] = (unsigned char)((data>>8)&0xFF);
    h[42] = (unsigned char)((data>>16)&0xFF); h[43] = (unsigned char)((data>>24)&0xFF);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s song.sid out.wav [seconds] [track]\n", argv[0]); return 1; }
    double seconds = argc > 3 ? atof(argv[3]) : 10.0;
    int track = argc > 4 ? atoi(argv[4]) : -1;

    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    rewind(in);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    size_t got = fread(buf, 1, (size_t)sz, in);
    fclose(in);
    if (got != (size_t)sz) return 1;

    if (loadSidFile(0, buf, (uint32_t)sz, 44100, argv[1], 0, 0, 0) != 0) {
        fprintf(stderr, "load failed\n"); return 1;
    }
    if (track < 0) track = 0;
    if (playTune((uint32_t)track, 0) != 0) { fprintf(stderr, "playTune failed\n"); return 1; }

    uint8_t simple = (uint8_t)!FileLoader::isExtendedSidFile();
    uint8_t speed = FileLoader::getCurrentSongSpeed();
    uint16_t chunk = (uint16_t)(44100 / vicFramesPerSecond());
    int16_t *synth = (int16_t *)malloc(sizeof(int16_t) * ((size_t)chunk * 2 + 1));

    double fps = vicFramesPerSecond();
    long frames = (long)(seconds * fps + 0.5);
    unsigned total = (unsigned)(frames * chunk);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    wav_header(out, 44100, total);

    short *mono = (short *)malloc(sizeof(short) * chunk);
    double sum = 0; int peak = 0; unsigned cnt = 0;
    for (long i = 0; i < frames; i++) {
        Core::runOneFrame(simple, speed, synth, 0, chunk);
        for (uint16_t k = 0; k < chunk; k++) {
            short v = synth[k * 2];
            mono[k] = v;
            sum += (double)v * v;
            if (abs(v) > peak) peak = abs(v);
        }
        fwrite(mono, 2, chunk, out);
        cnt += chunk;
    }
    fclose(out);
    free(mono); free(synth); free(buf);
    printf("frames=%u rms=%.0f peak=%d\n", cnt, sqrt(sum / (cnt ? cnt : 1)), peak);
    return 0;
}
