/*
 * build/lsfpref.cpp - render a .sid through stock libsidplayfp + reSIDfp.
 *
 * This is the reference harness the user asked for: build the untouched
 * upstream library, drive it with the documented API (config / load / play /
 * mix), and write a WAV. It deliberately contains no SID logic of its own.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/builders/residfp.h>

static void wav_header(FILE *f, unsigned rate, unsigned frames)
{
    unsigned char h[44];
    memset(h, 0, 44);
    unsigned data = frames * 2;
    memcpy(h, "RIFF", 4);
    h[4] = (unsigned char)((36 + data) & 0xFF);
    h[5] = (unsigned char)(((36 + data) >> 8) & 0xFF);
    h[6] = (unsigned char)(((36 + data) >> 16) & 0xFF);
    h[7] = (unsigned char)(((36 + data) >> 24) & 0xFF);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    h[24] = (unsigned char)(rate & 0xFF);
    h[25] = (unsigned char)((rate >> 8) & 0xFF);
    h[26] = (unsigned char)((rate >> 16) & 0xFF);
    h[27] = (unsigned char)((rate >> 24) & 0xFF);
    unsigned br = rate * 2;
    h[28] = (unsigned char)(br & 0xFF);
    h[29] = (unsigned char)((br >> 8) & 0xFF);
    h[30] = (unsigned char)((br >> 16) & 0xFF);
    h[31] = (unsigned char)((br >> 24) & 0xFF);
    h[32] = 2; h[34] = 16;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)(data & 0xFF);
    h[41] = (unsigned char)((data >> 8) & 0xFF);
    h[42] = (unsigned char)((data >> 16) & 0xFF);
    h[43] = (unsigned char)((data >> 24) & 0xFF);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s song.sid out.wav [seconds] [song] [model6581|8580]\n",
                argv[0]);
        return 1;
    }
    double seconds = argc > 3 ? atof(argv[3]) : 10.0;
    int song = argc > 4 ? atoi(argv[4]) : -1;
    bool force8580 = argc > 5 && !strcmp(argv[5], "8580");
    const unsigned SAMPLERATE = 44100;

    sidplayfp engine;
    std::unique_ptr<ReSIDfpBuilder> rs(new ReSIDfpBuilder("residfp"));


    std::unique_ptr<SidTune> tune(new SidTune(argv[1]));
    if (!tune->getStatus()) {
        fprintf(stderr, "tune error: %s\n", tune->statusString());
        return 1;
    }
    if (song < 0) song = tune->getInfo()->startSong() - 1;
    tune->selectSong((unsigned)song);

    SidConfig cfg;
    cfg.frequency = SAMPLERATE;
    cfg.samplingMethod = SidConfig::INTERPOLATE;
    cfg.sidEmulation = rs.get();
    cfg.defaultSidModel = force8580 ? SidConfig::MOS8580 : SidConfig::MOS6581;
    cfg.forceSidModel = false;
    /* Pin the power-on delay. Anything above MAX is randomised from a
     * time-seeded source, which shifts the phase of tunes that read a
     * free-running counter -- without this the same file renders differently
     * on every run and nothing can be compared byte-for-byte. */
    cfg.powerOnDelay = SidConfig::MAX_POWER_ON_DELAY;
    if (!engine.config(cfg)) {
        fprintf(stderr, "config error: %s\n", engine.error());
        return 1;
    }
    if (!engine.load(tune.get())) {
        fprintf(stderr, "load error: %s\n", engine.error());
        return 1;
    }

    engine.initMixer(true);            /* stereo */

    constexpr int CYCLES = 5000;       /* ~5 ms at 1 MHz */
    int bufSamples = engine.getBufSize(CYCLES);
    std::vector<short> buffer((size_t)bufSamples);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    wav_header(out, SAMPLERATE, 0);

    const unsigned want = (unsigned)(seconds * SAMPLERATE);
    unsigned written = 0;
    double sum = 0;
    int peak = 0;
    short mono[8192];

    while (written < want) {
        int res = engine.play(CYCLES);
        if (res < 0) { fprintf(stderr, "play error: %s\n", engine.error()); break; }
        unsigned mixed = engine.mix(buffer.data(), res);   /* stereo samples */
        unsigned frames = mixed / 2;
        if (frames == 0) continue;
        if (frames > 8192) frames = 8192;
        if (written + frames > want) frames = want - written;
        for (unsigned i = 0; i < frames; i++) {
            short v = buffer[i * 2];
            mono[i] = v;
            sum += (double)v * v;
            if (abs(v) > peak) peak = abs(v);
        }
        fwrite(mono, 2, frames, out);
        written += frames;
    }
    fclose(out);
    FILE *fix = fopen(argv[2], "r+b");
    if (fix) { wav_header(fix, SAMPLERATE, written); fclose(fix); }

    printf("song=%d model=%s frames=%u rms=%.0f peak=%d\n", song,
           force8580 ? "8580" : "6581", written,
           sqrt(sum / (written ? written : 1)), peak);
    return 0;
}
