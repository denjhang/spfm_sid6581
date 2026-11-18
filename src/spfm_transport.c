/*
 * spfm_transport.c - SPFM Light byte-stream transport over FTDI D2XX.
 *
 * Mirrors the transport used by the YM7129 reference driver: 64 KiB byte
 * buffer, 4 KiB USB chunks, 1.5 Mbaud, 2 ms latency timer. The difference is
 * that we expose Path A (4-byte) directly, because SID needs the two-phase
 * address/data write that Path A generates for free.
 */
#include "spfm_transport.h"

#include <string.h>
#include <stdlib.h>

#include "ftd2xx.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static FT_HANDLE s_handle = NULL;
static uint8_t   s_buf[SPFM_BUF_SIZE];
static size_t    s_len = 0;

static bool      s_dry_run = false;
static uint8_t  *s_cap = NULL;
static size_t    s_cap_len = 0;
static size_t    s_cap_cap = 0;

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */
static void cap_append(const uint8_t *p, size_t n)
{
    if (s_cap_len + n > s_cap_cap) {
        size_t want = s_cap_cap ? s_cap_cap : 8192;
        while (want < s_cap_len + n) want *= 2;
        uint8_t *np = (uint8_t *)realloc(s_cap, want);
        if (!np) return;
        s_cap = np;
        s_cap_cap = want;
    }
    memcpy(s_cap + s_cap_len, p, n);
    s_cap_len += n;
}

static void emit(const uint8_t *p, size_t n)
{
    if (s_dry_run) {
        cap_append(p, n);
        return;
    }
    if (!s_handle) return;
    if (s_len + n > SPFM_BUF_SIZE) spfm_flush();
    if (n > SPFM_BUF_SIZE) {
        /* oversized single write: chunk it */
        size_t off = 0;
        while (off < n) {
            size_t chunk = n - off;
            if (chunk > SPFM_BUF_SIZE) chunk = SPFM_BUF_SIZE;
            emit(p + off, chunk);
            off += chunk;
        }
        return;
    }
    memcpy(s_buf + s_len, p, n);
    s_len += n;
}

/* ------------------------------------------------------------------ */
/* device management                                                   */
/* ------------------------------------------------------------------ */
int spfm_list_devices(uint32_t *ids, int max, char (*descs)[64])
{
    DWORD num = 0;
    if (FT_CreateDeviceInfoList(&num) != FT_OK) return -1;
    int n = 0;
    for (DWORD i = 0; i < num && n < max; i++) {
        FT_DEVICE_LIST_INFO_NODE node;
        if (FT_GetDeviceInfoDetail(i, &node.Flags, &node.Type, &node.ID,
                                   &node.LocId, node.SerialNumber,
                                   node.Description, &node.ftHandle) != FT_OK)
            continue;
        ids[n] = (uint32_t)i;
        if (descs) {
            snprintf(descs[n], 64, "%.32s %.24s", node.Description, node.SerialNumber);
        }
        n++;
    }
    return n;
}

int spfm_open(int dev_index)
{
    s_len = 0;

    if (s_dry_run) return 0;

    FT_STATUS st = FT_Open(dev_index, &s_handle);
    if (st != FT_OK) {
        s_handle = NULL;
        fprintf(stderr, "spfm_open: FT_Open(%d) failed, status=%d\n",
                dev_index, (int)st);
        return -1;
    }

    FT_SetBaudRate(s_handle, 1500000);
    FT_SetDataCharacteristics(s_handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(s_handle, FT_FLOW_NONE, 0, 0);
    FT_SetTimeouts(s_handle, 100, 100);
    FT_SetLatencyTimer(s_handle, 2);
    FT_SetUSBParameters(s_handle, 65536, 65536);
    FT_Purge(s_handle, FT_PURGE_RX | FT_PURGE_TX);

    /* Same DTR/RTS dance the reference driver does before the first byte. */
    FT_SetDtr(s_handle); FT_SetRts(s_handle);
    spfm_delay_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_delay_ms(1);
    FT_SetRts(s_handle);
    spfm_delay_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_delay_ms(10);

    return 0;
}

void spfm_close(void)
{
    if (s_handle) {
        spfm_flush();
        uint8_t reset = 0xFE;
        DWORD written = 0;
        FT_Write(s_handle, &reset, 1, &written);
        spfm_delay_ms(50);
        FT_Close(s_handle);
        s_handle = NULL;
    }
    s_len = 0;
}

bool spfm_is_open(void)
{
    return s_dry_run || s_handle != NULL;
}

void spfm_send_reset(void)
{
    uint8_t reset = 0xFE;
    spfm_flush();
    if (s_dry_run) {
        cap_append(&reset, 1);
        return;
    }
    if (!s_handle) return;
    DWORD written = 0;
    FT_Write(s_handle, &reset, 1, &written);
    spfm_delay_ms(50);
}

/* ------------------------------------------------------------------ */
/* protocol                                                            */
/* ------------------------------------------------------------------ */
void spfm_write_path_a(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data)
{
    uint8_t cmd[4];
    cmd[0] = (uint8_t)(slot & 0x03);
    cmd[1] = (uint8_t)(port & 0x0F);   /* 0x00-0x0F selects Path A */
    cmd[2] = addr;
    cmd[3] = data;
    emit(cmd, 4);
}

void spfm_write_path_b(uint8_t slot, uint8_t n, uint8_t data)
{
    uint8_t cmd[3];
    cmd[0] = (uint8_t)(slot & 0x03);
    cmd[1] = (uint8_t)(0x80 | (n & 0x0F));
    cmd[2] = data;
    emit(cmd, 3);
}

void spfm_write_two_phase(uint8_t slot, uint8_t addr, uint8_t data)
{
    uint8_t cmd[6];
    cmd[0] = (uint8_t)(slot & 0x03); cmd[1] = 0x80; cmd[2] = addr;
    cmd[3] = (uint8_t)(slot & 0x03); cmd[4] = 0x81; cmd[5] = data;
    emit(cmd, 6);
}

void spfm_write_two_phase_rep(uint8_t slot, uint8_t addr, uint8_t data,
                              uint32_t repeat)
{
    uint8_t cmd[3];
    if (repeat == 0) repeat = 1;

    cmd[0] = (uint8_t)(slot & 0x03); cmd[1] = 0x80; cmd[2] = addr;
    emit(cmd, 3);
    cmd[1] = 0x81; cmd[2] = data;
    for (uint32_t i = 0; i < repeat; i++) emit(cmd, 3);
}

void spfm_write_split(uint8_t slot, uint8_t addr, uint8_t data,
                      uint32_t addr_rep, uint32_t gap, uint32_t data_rep)
{
    uint8_t cmd[3];
    uint8_t s = (uint8_t)(slot & 0x03);

    if (addr_rep == 0) addr_rep = 1;
    if (data_rep == 0) data_rep = 1;

    cmd[0] = s; cmd[1] = 0x80; cmd[2] = addr;
    for (uint32_t i = 0; i < addr_rep; i++) emit(cmd, 3);

    spfm_nop(gap);

    cmd[1] = 0x81; cmd[2] = data;
    for (uint32_t i = 0; i < data_rep; i++) emit(cmd, 3);
}

void spfm_write_split_gaps(uint8_t slot, uint8_t addr, uint8_t data,
                           uint32_t addr_rep, const uint32_t *gaps,
                           uint32_t data_rep)
{
    uint8_t cmd[3];
    uint8_t s = (uint8_t)(slot & 0x03);

    if (addr_rep == 0) addr_rep = 1;
    if (data_rep == 0) data_rep = 1;

    cmd[0] = s; cmd[1] = 0x80; cmd[2] = addr;
    for (uint32_t i = 0; i < addr_rep; i++) emit(cmd, 3);

    cmd[1] = 0x81; cmd[2] = data;
    for (uint32_t d = 0; d < data_rep; d++) {
        spfm_nop(gaps ? gaps[d] : 0);
        emit(cmd, 3);
    }
}

void spfm_write_phased(uint8_t slot, uint8_t addr, uint8_t data, uint32_t gap)
{
    uint8_t cmd[3];
    cmd[0] = (uint8_t)(slot & 0x03);
    cmd[1] = 0x80;              /* A0..A3 = 0, address phase */
    cmd[2] = addr;
    emit(cmd, 3);
    spfm_nop(gap);
    cmd[1] = 0x81;              /* A0 = 1, data phase */
    cmd[2] = data;
    emit(cmd, 3);
}

void spfm_nop(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = 0x80;
        emit(&b, 1);
    }
}

bool spfm_flush(void)
{
    if (s_dry_run) { s_len = 0; return true; }
    if (!s_handle || s_len == 0) { s_len = 0; return true; }

    size_t total = 0;
    while (total < s_len) {
        size_t chunk = s_len - total;
        if (chunk > 4096) chunk = 4096;
        DWORD written = 0;
        FT_STATUS st = FT_Write(s_handle, s_buf + total, (DWORD)chunk, &written);
        if (st != FT_OK || written != chunk) {
            fprintf(stderr, "spfm_flush: FT_Write failed (st=%d written=%lu of %lu)\n",
                    (int)st, (unsigned long)written, (unsigned long)chunk);
            s_len = 0;
            return false;
        }
        total += written;
    }
    s_len = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* delays                                                              */
/* ------------------------------------------------------------------ */
void spfm_delay_ms(uint32_t ms)
{
    if (s_dry_run) return;
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

void spfm_delay_us(uint32_t us)
{
    if (s_dry_run) return;
#ifdef _WIN32
    if (us >= 1000) {
        Sleep(us / 1000);
        return;
    }
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double ticks = (double)us * (double)freq.QuadPart / 1e6;
    do { QueryPerformanceCounter(&now); }
    while ((double)(now.QuadPart - start.QuadPart) < ticks);
#else
    usleep(us);
#endif
}

/* ------------------------------------------------------------------ */
/* dry-run capture                                                     */
/* ------------------------------------------------------------------ */
void spfm_dry_run_enable(bool on)
{
    s_dry_run = on;
    s_len = 0;
}

void spfm_capture_clear(void)
{
    s_cap_len = 0;
}

size_t spfm_capture_size(void)
{
    return s_cap_len;
}

const uint8_t *spfm_capture_data(void)
{
    return s_cap;
}

/*
 * Decode the captured byte stream back into commands. The firmware state
 * machine is reproduced here so a dry run tells us exactly what the bus will
 * do: which writes become two bus cycles, which bytes are delays, etc.
 */
void spfm_capture_dump(FILE *out)
{
    size_t i = 0;
    int cmdno = 0;
    while (i < s_cap_len) {
        uint8_t b0 = s_cap[i];
        if (b0 == 0x80) {
            size_t start = i;
            while (i < s_cap_len && s_cap[i] == 0x80) i++;
            fprintf(out, "  [%4d] NOP x%lu\n", cmdno++,
                    (unsigned long)(i - start));
            continue;
        }
        if (b0 == 0xFE) {
            fprintf(out, "  [%4d] RESET (0xFE) -> global /RES pulse, all slots\n", cmdno++);
            i++;
            continue;
        }
        if (i + 1 >= s_cap_len) {
            fprintf(out, "  [%4d] <truncated: 0x%02X>\n", cmdno++, b0);
            break;
        }
        uint8_t b1 = s_cap[i + 1];
        unsigned hi = (b1 >> 4) & 0x0F;

        if (hi == 0x0 && i + 3 < s_cap_len) {
            uint8_t addr = s_cap[i + 2], data = s_cap[i + 3];
            fprintf(out,
                    "  [%4d] A slot%u  BusWrite(A0=%u,D=0x%02X)  BusWrite(A0=1,D=0x%02X)"
                    "   => SID reg 0x%02X = 0x%02X\n",
                    cmdno++, b0 & 3u, b1 & 1u, addr, data, addr & 0x1F, data);
            i += 4;
        } else if (hi == 0x8 && (b1 & 0x0F) == 0 && i + 2 < s_cap_len) {
            /* Address phase; consume all the data phases that follow it. */
            uint8_t addr = s_cap[i + 2];
            size_t j = i + 3;
            unsigned nrep = 0;
            uint8_t last = 0;
            while (j + 2 < s_cap_len &&
                   s_cap[j] == b0 && s_cap[j + 1] == 0x81) {
                last = s_cap[j + 2];
                nrep++;
                j += 3;
            }
            if (nrep > 0) {
                fprintf(out,
                        "  [%4d] 2PH slot%u  addr(D=0x%02X) + data(A0=1,D=0x%02X) x%u"
                        "   => SID reg 0x%02X = 0x%02X\n",
                        cmdno++, b0 & 3u, addr, last, nrep,
                        addr & 0x1F, last);
                i = j;
            } else {
                fprintf(out,
                        "  [%4d] B slot%u  BusWrite(A0..A3=0x%X,D=0x%02X)\n",
                        cmdno++, b0 & 3u, b1 & 0x0F, addr);
                i += 3;
            }
        } else if (hi >= 0x8 && i + 2 < s_cap_len) {
            uint8_t data = s_cap[i + 2];
            fprintf(out,
                    "  [%4d] B slot%u  BusWrite(A0..A3=0x%X,D=0x%02X)\n",
                    cmdno++, b0 & 3u, b1 & 0x0F, data);
            i += 3;
        } else {
            fprintf(out, "  [%4d] ? slot%u byte1=0x%02X (firmware drops this)\n",
                    cmdno++, b0 & 3u, b1);
            i += 2;
        }
    }
}
