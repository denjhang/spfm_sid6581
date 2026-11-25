#include "scc.h"
#include <string.h>

void scc_init(scc_state_t *s, uint32_t clock_hz) {
    memset(s, 0, sizeof(*s));
    s->clock = clock_hz;
    s->rate = 44100;
}

void scc_reset(scc_state_t *s) {
    for (int i = 0; i < SCC_CHANS; i++) {
        s->ch[i].counter = 0;
        s->ch[i].frequency = 0;
        s->ch[i].volume = 0;
        s->ch[i].key = 0;
        memset(s->ch[i].waveram, 0, SCC_WAVELEN);
    }
    s->test = 0;
    s->cur_reg = 0;
}

void scc_write(scc_state_t *s, uint8_t port, uint8_t data) {
    if (port & 1) {
        // Write data — port>>1 selects function
        switch (port >> 1) {
        case 0x00:
        case 0x04: {
            uint8_t offset = s->cur_reg;
            if (s->test & 0x40) return;
            if (!s->mode_plus) {
                if (offset >= 0x60) {
                    s->ch[3].waveram[offset & 0x1f] = (int8_t)data;
                    s->ch[4].waveram[offset & 0x1f] = (int8_t)data;
                } else {
                    s->ch[offset >> 5].waveram[offset & 0x1f] = (int8_t)data;
                }
            } else {
                s->ch[offset >> 5].waveram[offset & 0x1f] = (int8_t)data;
            }
            break;
        }
        case 0x01: {
            uint8_t offset = s->cur_reg;
            int ch = offset >> 1;
            if (ch < SCC_CHANS) {
                if (offset & 1)
                    s->ch[ch].frequency = (s->ch[ch].frequency & 0x00FF) | ((uint16_t)(data & 0x0F) << 8);
                else
                    s->ch[ch].frequency = (s->ch[ch].frequency & 0x0F00) | data;
                s->ch[ch].counter &= 0xFFFF0000u;
                if (s->test & 0x20)
                    s->ch[ch].counter = 0xFFFFFFFF;
                else if (s->ch[ch].frequency < 9)
                    s->ch[ch].counter |= ((1 << SCC_FREQ_BITS) - 1);
            }
            break;
        }
        case 0x02: {
            int ch = s->cur_reg & 0x07;
            if (ch < SCC_CHANS)
                s->ch[ch].volume = data & 0x0F;
            break;
        }
        case 0x03:
            for (int i = 0; i < SCC_CHANS; i++)
                s->ch[i].key = (data >> i) & 1;
            break;
        case 0x05:
            s->test = data;
            break;
        }
    } else {
        // Latch register select
        s->cur_reg = data;
    }
}

int8_t scc_render(scc_state_t *s) {
    int16_t mix = 0;
    for (int i = 0; i < SCC_CHANS; i++) {
        scc_channel_t *c = &s->ch[i];
        if (c->frequency > 8) {
            uint32_t step = (uint32_t)(
                ((uint64_t)(s->clock >> 1) << (SCC_FREQ_BITS + 1)) /
                ((uint32_t)(c->frequency + 1) * s->rate)
            );
            c->counter += step;
            if (c->key) {
                uint32_t offs = (c->counter >> SCC_FREQ_BITS) & 0x1F;
                int16_t smpl = (int16_t)c->waveram[offs] * c->volume;
                smpl >>= 4;
                mix += smpl;
            }
        }
    }
    if (mix > 127) mix = 127;
    if (mix < -128) mix = -128;
    return (int8_t)mix;
}
