/*
 * spfm_transport.h - SPFM Light byte-stream transport over FTDI D2XX.
 *
 * The host only ever pushes bytes; the STM32 firmware turns them into bus
 * cycles. Two command forms matter for this project:
 *
 *   Path A (4 bytes): [slot, 0x00, addr, data]
 *       BusWrite(slot, A0=0, D=addr)
 *       BusWrite(slot, A0=1, D=data)
 *
 *   Path B (3 bytes): [slot, 0x80 | (A0..A3), data]
 *       BusWrite(slot, A0..A3, D=data)
 *
 * Byte 0x80 in first position is a NOP/delay tick, 0xFE is the global reset.
 */
#ifndef SPFM_TRANSPORT_H
#define SPFM_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPFM_MAX_SLOT   3
#define SPFM_BUF_SIZE   (1024 * 64)

/* Device enumeration. Returns count, writes up to max ids. */
int  spfm_list_devices(uint32_t *ids, int max, char (*descs)[64]);

/* Open device by FTDI index. Returns 0 on success. */
int  spfm_open(int dev_index);
void spfm_close(void);
bool spfm_is_open(void);

/* Global hardware reset (0xFE). Not per-slot: every chip on every slot is
 * reset at the same time. */
void spfm_send_reset(void);

/* Path A: address phase (A0=0) then data phase (A0=1). 4 bytes. */
void spfm_write_path_a(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data);

/* Path B: single bus write, A0..A3 = (n & 0x0F). 3 bytes. */
void spfm_write_path_b(uint8_t slot, uint8_t n, uint8_t data);

/* Two Path B commands emitted as one 6-byte record:
 *
 *     [slot, 0x80, addr] [slot, 0x81, data]
 *
 * This is exactly how the YM7129 driver does it. The address phase and the
 * data phase are separate firmware commands, so the host controls the timing
 * between them by flushing and sleeping. */
void spfm_write_two_phase(uint8_t slot, uint8_t addr, uint8_t data);

/* Address phase followed by `repeat` identical data phases, all in one record:
 *
 *     [slot, 0x80, addr] [slot, 0x81, data] x repeat
 *
 * The address stays latched in the 74LS374 across every data phase, so this
 * costs one address phase and N write pulses. This is how a too-short /WR
 * pulse is made reliable: the SID only latches when PHI2 rises while /CS and
 * R/W are both low, and one 200 ns pulse inside a 1 MHz PHI2 can miss the
 * sampling edge entirely. N pulses spread over N x ~20 us always cover
 * several PHI2 edges. */
void spfm_write_two_phase_rep(uint8_t slot, uint8_t addr, uint8_t data,
                              uint32_t repeat);

/* Two Path B commands with `gap` NOP ticks between them. */
/* Address phase repeated `addr_rep` times, then `gap` NOP ticks, then the data
 * phase repeated `data_rep` times. All of it goes out as one record, so a
 * single USB transfer carries the whole write.
 *
 *     [slot,0x80,addr] x addr_rep   [0x80 x gap]   [slot,0x81,data] x data_rep
 *
 * The 74LS374 keeps its address latched across every repeat, so repeating the
 * address phase is harmless and repeating the data phase is harmless too (the
 * SID register just gets the same value several times). Splitting the two
 * counts lets the caller find out which phase is the one losing cycles. */
void spfm_write_split(uint8_t slot, uint8_t addr, uint8_t data,
                      uint32_t addr_rep, uint32_t gap, uint32_t data_rep);

/* Same as spfm_write_split, but a different gap is used before each data
 * write. `gaps[d]` is emitted before the d-th data phase.
 *
 * This is what actually decorrelates the attempts. Back-to-back data phases
 * are 20.0 us apart (3 bytes at 1.5 Mbaud) which is an exact multiple of the
 * 1 us PHI2 period, so every one of them samples the same PHI2 phase and D
 * repeats behave exactly like one. Varying the gap between them gives each
 * attempt an independent chance of covering a PHI2 rising edge. */
void spfm_write_split_gaps(uint8_t slot, uint8_t addr, uint8_t data,
                           uint32_t addr_rep, const uint32_t *gaps,
                           uint32_t data_rep);

void spfm_write_phased(uint8_t slot, uint8_t addr, uint8_t data, uint32_t gap);

/* Append n NOP bytes (0x80), each costs one firmware loop tick. */
void spfm_nop(uint32_t n);

/* Push everything buffered to USB. Returns false on write error. */
bool spfm_flush(void);

/* Bytes still sitting in the FTDI driver's transmit queue. Zero when the
 * device is closed or in dry-run mode, so a caller can treat it as "drained".
 *
 * This is the counterpart of MDPlayer's IsBufferEmpty(): without it the player
 * has no way to tell "the bytes are queued" from "the bytes are on the wire",
 * and a dense stream just fills the queue until the driver starts refusing
 * them. */
size_t spfm_tx_pending(void);

/* Flush, then wait until the transmit queue drops below `low_water` bytes or
 * `deadline_us` passes (pass 0 for "no deadline"). Returns the byte count still
 * pending when it gave up, so the caller can report a sustained backlog.
 *
 * The wait is coalesced: it queries the queue once every ~200 us rather than
 * spinning, and it never sleeps past the deadline. */
size_t spfm_flush_drain(size_t low_water, int64_t deadline_us);

/* Wall-clock delay for long waits; the byte buffer is flushed first. */
void spfm_delay_ms(uint32_t ms);
void spfm_delay_us(uint32_t us);

/* Monotonic microsecond clock, plus a wait that targets an absolute deadline.
 *
 * Replaying a .sid means pushing each register write out at the wall-clock
 * time recorded in the stream. Sleeping "for N us" per event would accumulate
 * the scheduler's error over the whole song, so the player keeps one start
 * timestamp and waits until start + t_event; the error then stays bounded
 * instead of growing. Returns immediately if the deadline already passed.
 *
 * In dry-run mode now_us() still returns the real clock (so the player's
 * pacing code runs for real) while the wait itself is skipped. */
int64_t spfm_now_us(void);
void    spfm_wait_until_us(int64_t deadline_us);

/* ---- Dry-run capture (no hardware) ---------------------------------- *
 * When enabled, nothing is sent to USB; every byte is appended to an
 * in-memory log so the generated stream can be inspected or asserted. */
void        spfm_dry_run_enable(bool on);
void        spfm_capture_clear(void);
size_t      spfm_capture_size(void);
const uint8_t *spfm_capture_data(void);
/* Pretty-print captured commands as decoded lines. */
void        spfm_capture_dump(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* SPFM_TRANSPORT_H */
