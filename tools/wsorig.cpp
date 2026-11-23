/* Renders a .sid with upstream WebSid's OWN audio entry point
 * (computeAudioSamples / getSoundBuffer), i.e. exactly what the browser and
 * the Raspberry Pi driver call. No custom clocking loop at all. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

extern "C" {
uint32_t loadSidFile(uint32_t is_mus, void *in_buffer, uint32_t in_buf_size,
                     uint32_t sample_rate, char *filename, void *basic_ROM,
                     void *char_ROM, void *kernal_ROM);
uint32_t playTune(uint32_t selected_track, uint32_t trace_sid);
int32_t  computeAudioSamples();
char    *getSoundBuffer();
uint32_t getSoundBufferLen();
uint32_t getSampleRate();
uint8_t  getDigiType();
const char *getDigiTypeDesc();
uint16_t getDigiRate();

}

static void wav_header(FILE *f, unsigned rate, unsigned frames)
{
    unsigned char h[44]; memset(h, 0, 44);
    unsigned data = frames * 2;
    memcpy(h, "RIFF", 4);
    h[4]=(unsigned char)((36+data)&0xFF); h[5]=(unsigned char)(((36+data)>>8)&0xFF);
    h[6]=(unsigned char)(((36+data)>>16)&0xFF); h[7]=(unsigned char)(((36+data)>>24)&0xFF);
    memcpy(h+8,"WAVEfmt ",8); h[16]=16; h[20]=1; h[22]=1;
    h[24]=(unsigned char)(rate&0xFF); h[25]=(unsigned char)((rate>>8)&0xFF);
    h[26]=(unsigned char)((rate>>16)&0xFF); h[27]=(unsigned char)((rate>>24)&0xFF);
    unsigned br=rate*2;
    h[28]=(unsigned char)(br&0xFF); h[29]=(unsigned char)((br>>8)&0xFF);
    h[30]=(unsigned char)((br>>16)&0xFF); h[31]=(unsigned char)((br>>24)&0xFF);
    h[32]=2; h[34]=16; memcpy(h+36,"data",4);
    h[40]=(unsigned char)(data&0xFF); h[41]=(unsigned char)((data>>8)&0xFF);
    h[42]=(unsigned char)((data>>16)&0xFF); h[43]=(unsigned char)((data>>24)&0xFF);
    fwrite(h,1,44,f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s song.sid out.wav [seconds] [track]\n", argv[0]); return 1; }
    double seconds = argc > 3 ? atof(argv[3]) : 10.0;
    int track = argc > 4 ? atoi(argv[4]) : -1;

    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cannot open\n"); return 1; }
    fseek(in,0,SEEK_END); long sz=ftell(in); rewind(in);
    unsigned char *buf=(unsigned char*)malloc((size_t)sz);
    if (fread(buf,1,(size_t)sz,in)!=(size_t)sz) return 1;
    fclose(in);

    if (loadSidFile(0, buf, (uint32_t)sz, 44100, argv[1], 0, 0, 0) != 0) {
        fprintf(stderr,"load failed\n"); return 1;
    }
    if (playTune(track < 0 ? 0 : (uint32_t)track, 0) != 0) {
        fprintf(stderr,"playTune failed\n"); return 1;
    }

    unsigned rate = getSampleRate();
    FILE *out=fopen(argv[2],"wb");
    if (!out) { fprintf(stderr,"cannot write\n"); return 1; }
    /* header written after the fact: fill later */
    wav_header(out, rate, 0);

    long want = (long)(seconds * rate);
    long written = 0;
    double sum = 0; int peak = 0;
    short *mono = (short*)malloc(sizeof(short) * 4096);

    while (written < want) {
        int32_t n = computeAudioSamples();          /* upstream's own entry point */
        if (n <= 0) break;
        uint32_t len = getSoundBufferLen();
        if (len == 0) break;
        const int16_t *sb = (const int16_t*)getSoundBuffer();
        uint32_t take = len;
        if ((long)take > want - written) take = (uint32_t)(want - written);
        for (uint32_t i=0;i<take;i++) {
            short v = sb[i*2];                      /* interleaved, left = SID out */
            mono[i] = v;
            sum += (double)v*v;
            if (abs(v) > peak) peak = abs(v);
        }
        fwrite(mono, 2, take, out);
        written += take;
    }
    fclose(out);

    /* patch the sizes */
    FILE *fix=fopen(argv[2],"r+b");
    if (fix) { wav_header(fix, rate, (unsigned)written); fclose(fix); }

    printf("digi type=%d (%s) rate=%u\n", getDigiType(), getDigiTypeDesc(), getDigiRate());
    printf("samples=%ld rms=%.0f peak=%d\n", written,
           sqrt(sum/(written?written:1)), peak);
    free(mono); free(buf);
    return 0;
}
