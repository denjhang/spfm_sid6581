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
#include <mmsystem.h>
#else
#include <unistd.h>
#include <time.h>
#endif

static FT_HANDLE s_handle = NULL;
static uint8_t   s_buf[SPFM_BUF_SIZE];
static size_t    s_len = 0;

/* Transfer accounting. A register stream can be perfectly correct byte for
 * byte and still sound wrong because it was handed to the driver as thousands
 * of 4-byte writes: FT232R does one bulk transaction per 1 ms USB frame, so
 * frequency of FT_Write, not total bytes, is what has to stay under ~1000/s.
 * Counted in both real and dry-run mode, since dry-run is how we check a
 * change without the board. */
static unsigned long s_writes = 0;
static unsigned long s_write_bytes = 0;
static size_t        s_max_write = 0;

static bool      s_dry_run = false;
static uint8_t  *s_cap = NULL;
static size_t    s_cap_len = 0;
static size_t    s_cap_cap = 0;

#ifdef _WIN32
static bool      s_timer_res_raised = false;
static bool      s_boosted = false;

/* Wait primitive selection, ported from Denjhang_Music_Player v16
 * (windows/ym2608/ym2608_window.cpp timer_sleep_1ms and
 * windows/gigatron/winmm.c audio-thread boost). Default is H-Prec: a
 * persistent high-resolution waitable timer, which is the one mode the
 * reference gets wrong (it recreates a default-resolution timer per call). */
static int       s_timer_mode = SPFM_TIMER_HPREC;
static HANDLE    s_hires_timer = NULL;   /* persistent, high-resolution */
static HANDLE    s_mm_event = NULL;      /* vgmplay: periodic 1 ms timer event */
static MMRESULT  s_mm_timer = 0;

static void     disable_power_throttling(void);
static HANDLE   hires_timer_create(void);
#endif

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
    if (!s_dry_run && !s_handle) return;

    /* The 64 KiB buffer is part of the model, not an implementation detail: it
     * is what forces a flush every ~437 ms of paced bytes. Dry-run runs the
     * same check so its transfer count matches the board's. */
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
    if (s_dry_run) cap_append(p, n);
    else           memcpy(s_buf + s_len, p, n);
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

    /* Drop the scheduler tick from 15.6 ms to 1 ms for as long as the device is
     * open. Without this a Sleep(4) inside the playback wait can block for 15.6
     * ms, which is most of a C64 frame - the stream then arrives late and the
     * music drags. Same reasoning as the reference player's hybrid timer. */
#ifdef _WIN32
    timeBeginPeriod(1);
    s_timer_res_raised = true;

    /* Keep the process out of EcoQoS and give the wait primitive a
     * high-resolution timer. Without the first, Windows demotes the playback
     * thread the moment the window goes to the background, which is precisely
     * the "switch window and SPFM stutters" report. */
    disable_power_throttling();
    if (!s_hires_timer) s_hires_timer = hires_timer_create();
#endif

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
#ifdef _WIN32
    if (s_timer_res_raised) {
        timeEndPeriod(1);
        s_timer_res_raised = false;
    }
    if (s_mm_timer) { timeKillEvent(s_mm_timer); s_mm_timer = 0; }
    if (s_mm_event) { CloseHandle(s_mm_event); s_mm_event = NULL; }
    if (s_hires_timer) { CloseHandle(s_hires_timer); s_hires_timer = NULL; }
#endif
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
    if (s_len > s_max_write) s_max_write = s_len;
    if (s_len) { s_writes++; s_write_bytes += s_len; }

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

void spfm_flush_stats_reset(void)
{
    s_writes = 0;
    s_write_bytes = 0;
    s_max_write = 0;
}

void spfm_flush_stats(unsigned long *writes, unsigned long *bytes,
                      size_t *max_write)
{
    if (writes)    *writes = s_writes;
    if (bytes)     *bytes = s_write_bytes;
    if (max_write) *max_write = s_max_write;
}

/* ------------------------------------------------------------------ */
/* delays                                                              */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/* CreateWaitableTimerExW is not in the MinGW import headers unless a newer
 * _WIN32_WINNT is set, and the high-resolution flag has no constant, so both
 * are resolved at runtime. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
typedef HANDLE (WINAPI *create_waitable_timer_ex_w_t)(
    LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

static HANDLE hires_timer_create(void)
{
    HANDLE h = NULL;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        create_waitable_timer_ex_w_t fn = (create_waitable_timer_ex_w_t)
            (void *)GetProcAddress(k32, "CreateWaitableTimerExW");
        if (fn)
            h = fn(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                   TIMER_ALL_ACCESS);
    }
    if (!h) h = CreateWaitableTimerW(NULL, FALSE, NULL);
    return h;
}

/* Reference: sleep_hybrid_us() in ym2608_window.cpp - Sleep() for the bulk,
 * busy-wait the last millisecond. */
static void sleep_hybrid_us(uint32_t us)
{
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    LONGLONG target = start.QuadPart + (freq.QuadPart * (LONGLONG)us) / 1000000;
    if (us > 1000) Sleep((us - 1000) / 1000);
    do { QueryPerformanceCounter(&now); } while (now.QuadPart < target);
}

/* Reference: sleep_mm_timer_us() in ym2608_window.cpp - timeSetEvent one-shot
 * with TIME_CALLBACK_EVENT_SET, wait on the event. */
static void sleep_mm_timer_us(uint32_t us)
{
    unsigned ms = us / 1000;
    if (ms < 1) ms = 1;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev) { Sleep(ms); return; }
    MMRESULT tid = timeSetEvent(ms, 1, (LPTIMECALLBACK)ev, 0,
                                TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
    if (tid) {
        WaitForSingleObject(ev, us / 1000 + 20);
        timeKillEvent(tid);
    } else {
        Sleep(ms);
    }
    CloseHandle(ev);
}

/* H-Prec: persistent high-resolution waitable timer. The reference recreates a
 * default-resolution timer on every call (CreateWaitableTimer + SetWaitableTimer
 * inside sleep_precise_us), which costs a kernel object per sleep and inherits
 * the 15.6 ms tick; a single high-resolution timer created once does the same
 * job for the whole session. */
static void sleep_hprec_us(uint32_t us)
{
    if (!s_hires_timer) { Sleep(us / 1000); return; }
    LARGE_INTEGER due;
    due.QuadPart = -((LONGLONG)us * 10);          /* 100 ns units, relative */
    if (!SetWaitableTimer(s_hires_timer, &due, 0, NULL, NULL, FALSE)) {
        Sleep(us / 1000);
        return;
    }
    WaitForSingleObject(s_hires_timer, us / 1000 + 20);
}

/* VGMPlay: 1 ms periodic multimedia timer driving an auto-reset event. */
static void sleep_vgmplay_us(uint32_t us)
{
    if (!s_mm_event) {
        s_mm_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (s_mm_event)
            s_mm_timer = timeSetEvent(1, 1, (LPTIMECALLBACK)s_mm_event, 0,
                                      TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
    }
    if (!s_mm_timer) { Sleep((us + 999) / 1000); return; }
    DWORD ms = (us + 999) / 1000;
    WaitForSingleObject(s_mm_event, ms ? ms : 1);
}
#endif /* _WIN32 */

int spfm_timer_set(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "hprec") || !strcmp(name, "h-prec") || !strcmp(name, "0"))
        s_timer_mode = SPFM_TIMER_HPREC;
    else if (!strcmp(name, "hybrid") || !strcmp(name, "1"))
        s_timer_mode = SPFM_TIMER_HYBRID;
    else if (!strcmp(name, "mmtimer") || !strcmp(name, "mm-timer") || !strcmp(name, "2"))
        s_timer_mode = SPFM_TIMER_MMTIMER;
    else if (!strcmp(name, "vgmplay") || !strcmp(name, "3"))
        s_timer_mode = SPFM_TIMER_VGMPLAY;
    else
        return -1;
    return 0;
}

const char *spfm_timer_name(void)
{
    switch (s_timer_mode) {
    case SPFM_TIMER_HYBRID:  return "hybrid";
    case SPFM_TIMER_MMTIMER: return "mmtimer";
    case SPFM_TIMER_VGMPLAY: return "vgmplay";
    default:                 return "hprec";
    }
}

int spfm_timer_mode(void) { return s_timer_mode; }

void spfm_thread_boost_begin(void)
{
#ifdef _WIN32
    /* MMCSS "Pro Audio" is what keeps a playback thread scheduled while a
     * foreground window owns the CPU; fall back to the highest priority. */
    typedef HANDLE (WINAPI *av_set_mm_thread_t)(LPCWSTR, LPDWORD, LPDWORD);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (avrt) {
        av_set_mm_thread_t fn = (av_set_mm_thread_t)
            (void *)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        DWORD idx = 0;
        if (fn && fn(L"Pro Audio", &idx, &idx)) { s_boosted = true; return; }
        FreeLibrary(avrt);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    s_boosted = true;
#endif
}

void spfm_thread_boost_end(void)
{
#ifdef _WIN32
    if (s_boosted) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    s_boosted = false;
#endif
}

/* Exclude this process from Windows power throttling (EcoQoS), which otherwise
 * drops the playback thread's scheduling class as soon as the window is in the
 * background - exactly the "switch window and the SPFM stutters" symptom.
 * SetProcessInformation is resolved at runtime: MinGW only imports it when
 * _WIN32_WINNT >= 0x0602, and this build targets plain Win32. */
static void disable_power_throttling(void)
{
#ifdef _WIN32
    typedef BOOL (WINAPI *set_process_information_t)(
        HANDLE, int, void *, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    set_process_information_t fn = (set_process_information_t)
        (void *)GetProcAddress(k32, "SetProcessInformation");
    if (!fn) return;
    PROCESS_POWER_THROTTLING_STATE st;
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                     PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    st.StateMask = 0;
    fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st);
#endif
}

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
    switch (s_timer_mode) {
    case SPFM_TIMER_HYBRID:  sleep_hybrid_us(us); break;
    case SPFM_TIMER_MMTIMER: sleep_mm_timer_us(us); break;
    case SPFM_TIMER_VGMPLAY: sleep_vgmplay_us(us); break;
    default:                 sleep_hprec_us(us); break;
    }
#else
    usleep(us);
#endif
}

int64_t spfm_now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    /* Integer maths keeps the replay timeline free of float drift over a
     * multi-minute song. */
    return (int64_t)(now.QuadPart / freq.QuadPart) * 1000000
         + (int64_t)((now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

void spfm_wait_until_us(int64_t deadline_us)
{
    if (s_dry_run) return;

    /* Absolute-deadline wait. The shape mirrors libvgm's playback model: the
     * clock is read every pass and the stream is emitted up to "now", so a
     * late wake-up bunches the writes instead of shifting the whole song.
     *
     * The last hop uses the selected timer primitive (spfm_delay_us), which is
     * H-Prec by default: a persistent high-resolution waitable timer. Sleep()
     * alone would round a 4 ms wait up to the 15.6 ms default tick; the 1 ms
     * timeBeginPeriod in spfm_open() plus a high-resolution timer removes that.
     *
     * vgmplay mode deliberately does not spin - it waits on the 1 ms periodic
     * timer event, which is what the reference "VGMPlay" mode does and keeps
     * the CPU idle at the cost of ~1 ms jitter. */
    for (;;) {
        int64_t left = deadline_us - spfm_now_us();
        if (left <= 0) return;
        if (left > 1500) spfm_delay_ms((uint32_t)((left - 1000) / 1000));
        else             spfm_delay_us((uint32_t)left);
    }
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
