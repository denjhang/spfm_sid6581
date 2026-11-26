/*
 * rpfm_link.h - host side of the RPFM HID protocol, for the RP2350 SID player.
 *
 * Copied from RPFM_Player/src/rpfm_hid.cpp (YM2163-Midi project) and changed in
 * two ways:
 *
 *   1. It is C, not C++, because the player it serves (spfmplayer.c) is C.
 *   2. It carries SID register events instead of YM2413/AY8910 writes.
 *
 * The frame format itself is unchanged and comes straight from the firmware
 * header, so there is one definition of it:
 *
 *     [CMD, SEQ, LEN, payload.., CRC8]      64 bytes, report id 0 in front
 *
 * The SID needs something the YM chips did not: time. A SID register write has
 * to land at a specific microsecond, and the SPFM link could not say that (it
 * had only Path A/B/NOP bytes and no clock). Here the event carries a delta and
 * the firmware applies it against an absolute deadline, so USB jitter never
 * reaches the chip. That is why this protocol is the whole reason the RP2350
 * port exists.
 */
#ifndef RPFM_LINK_H
#define RPFM_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  ack_seq;
    uint8_t  status;
    uint16_t buf_level;   /* events queued in the firmware */
    uint32_t tick;
} rpfm_resp_t;

/* Find the player and open it. Returns 0 on success, -1 if no board is
 * present. Does not reset the board: the caller decides when the SID is
 * cleared. */
int  rpfm_link_open(void);
void rpfm_link_close(void);
bool rpfm_link_is_open(void);

/* Command codes, copied from the firmware's src/protocol.h. Only the ones this
 * player uses are repeated here; the rest stay in the firmware header. */
#define CMD_RESET      0x03
#define CMD_BOOTSEL    0x20
#define CMD_SID_EVENTS 0x30
#define CMD_PHI2       0x31
#define CMD_SID_WRITE  0x32
#define CMD_SID_STEP   0x33
#define CMD_SID_START  0x34
#define CMD_SID_STOP   0x35

/* Raw frame. `plen` is the payload length, at most 60. */
bool rpfm_link_send(uint8_t cmd, const uint8_t *payload, uint8_t plen,
                    rpfm_resp_t *resp);

bool rpfm_link_reset(void);          /* CMD_RESET: IC# pulse + clear the queue */
bool rpfm_link_bootsel(void);        /* CMD_BOOTSEL: hands-free flashing */
bool rpfm_set_phi2(int ntsc);        /* CMD_PHI2 */
bool rpfm_set_sid_step(int hold);    /* CMD_SID_STEP: data-phase hold, 1..32 PHI2 periods */
bool rpfm_sid_start(void);           /* CMD_SID_START: release the playback gate */
bool rpfm_sid_stop(void);            /* CMD_SID_STOP: shut the gate, drop the
                                      * queue, forget the timeline. The track
                                      * boundary command - no IC# pulse, so it
                                      * is fast enough to use between two files
                                      * of a playlist. Always follow it with a
                                      * prefill and then rpfm_sid_start(). */

/* Events queued in the firmware, read through the control endpoint. Returns
 * 0 when the board is not answering. */
uint16_t rpfm_link_queue_level(void);

/* ---- SID event stream ------------------------------------------------- *
 * Events are batched into CMD_SID_EVENTS frames ([dt_lo,dt_hi,addr,data] x N,
 * N <= 15). The delta is relative to the previous event, in microseconds, and
 * is applied by the firmware against an absolute deadline. A delta longer than
 * 65535 us cannot be expressed in the field, so it is split: the filler events
 * carry addr = 0xFF, which the firmware treats as "wait, do not write". */

void rpfm_sid_stream_begin(void);    /* reset the batching state */
bool rpfm_sid_push(uint16_t dt_us, uint8_t reg, uint8_t val);
bool rpfm_sid_flush(void);

/* One write with no timestamp: goes out immediately, in order. Used for the
 * mute-all and clear-registers housekeeping around a track change, where the
 * player must not wait for a deadline. */
bool rpfm_sid_write_now(uint8_t reg, uint8_t val);

/* Monotonic microsecond clock (QPC). The player uses one start stamp and waits
 * against absolute deadlines, exactly like the SPFM player does, so a slow
 * frame cannot make the tune drift. */
int64_t rpfm_now_us(void);

#endif /* RPFM_LINK_H */
