/*
 * sidmodel.h - software MOS 6581/8580 model, for auditioning a sidvgm stream
 * without any hardware.
 *
 * Scope: this is an approximation, not a silicon replica. Oscillators, ADSR,
 * pulse width, noise LFSR, ring modulation, hard sync and the state-variable
 * filter are modelled with the same constants the reference emulators use
 * (WebSid's cycle-by-cycle envelope counters, reSID's period table), but the
 * 6581's nonlinear filter and combined-waveform quirks are not reproduced
 * bit-accurately. It is meant to answer "did this stream record the right
 * notes, envelopes and timing", not to replace the real chip.
 *
 * Everything is clocked per PHI2 cycle, so the model runs at the chip's own
 * rate (985248 Hz PAL) and downsamples to the requested output rate.
 */
#ifndef SIDMODEL_H
#define SIDMODEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sid_model sid_model_t;

/* clock_hz: PHI2 (985248 PAL / 1022730 NTSC). model8580 picks the filter
 * curve. sample_rate is the output rate; the model resamples internally. */
sid_model_t *sid_model_new(double clock_hz, int model8580, double sample_rate);
void         sid_model_free(sid_model_t *m);
void         sid_model_reset(sid_model_t *m);

/* $D400-$D418 register write, in the order the stream has them. */
void         sid_model_write(sid_model_t *m, uint8_t reg, uint8_t val);

/* Render `frames` mono samples into buf. Values are the chip's own scale
 * (roughly 0..12000 for full polyphony), not normalised. */
void         sid_model_render(sid_model_t *m, short *buf, size_t frames);

/* Filter bypass, for comparing against the raw voices. */
void         sid_model_set_filter(sid_model_t *m, int on);

#ifdef __cplusplus
}
#endif

#endif /* SIDMODEL_H */
