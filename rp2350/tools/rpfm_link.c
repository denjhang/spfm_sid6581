/*
 * rpfm_link.c - host side of the RPFM HID protocol (SID variant).
 *
 * The device discovery and the frame write are lifted from
 * RPFM_Player/src/rpfm_hid.cpp, which is the version that works on this
 * hardware: WriteFile on the interrupt OUT endpoint (5-10x faster than
 * HidD_SetOutputReport, which goes through a control transfer) and
 * HidD_GetInputReport for the uplink. Only the payloads are new.
 */
#include "rpfm_link.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <string.h>
#include <stdio.h>

#define RPFM_VID 0x2E8A
#define RPFM_PID 0x1090

/* How many events one CMD_SID_EVENTS frame carries. 15 * 4 = 60 bytes, which is
 * exactly the payload limit. */
#define EV_PER_FRAME 15

static HANDLE    s_handle = INVALID_HANDLE_VALUE;
static uint8_t   s_seq = 0;
static LARGE_INTEGER s_qpc_freq;

static uint8_t   s_batch[EV_PER_FRAME * 4];
static unsigned  s_batch_n = 0;      /* events currently batched */

static uint8_t crc8_frame(const uint8_t *d, int len)
{
    uint8_t c = 0;
    for (int i = 0; i < len; i++) {
        c ^= d[i];
        for (int j = 0; j < 8; j++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

int rpfm_link_open(void)
{
    if (s_handle != INVALID_HANDLE_VALUE) return 0;

    QueryPerformanceFrequency(&s_qpc_freq);

    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsA(&g, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return -1;

    SP_DEVICE_INTERFACE_DATA ifc;
    ifc.cbSize = sizeof(ifc);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifc); i++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifc, NULL, 0, &req, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = malloc(req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(di, &ifc, det, req, NULL, NULL)) {
            free(det);
            continue;
        }

        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        free(det);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES a;
        a.Size = sizeof(a);
        if (HidD_GetAttributes(h, &a) && a.VendorID == RPFM_VID &&
            a.ProductID == RPFM_PID) {
            SetupDiDestroyDeviceInfoList(di);
            s_handle = h;
            return 0;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(di);
    return -1;
}

void rpfm_link_close(void)
{
    if (s_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(s_handle);
        s_handle = INVALID_HANDLE_VALUE;
    }
}

bool rpfm_link_is_open(void)
{
    return s_handle != INVALID_HANDLE_VALUE;
}

bool rpfm_link_send(uint8_t cmd, const uint8_t *payload, uint8_t plen,
                    rpfm_resp_t *resp)
{
    if (s_handle == INVALID_HANDLE_VALUE) return false;
    if (plen > 60) return false;

    /* 65 bytes: report id 0, then the 64-byte frame. */
    uint8_t buf[65];
    memset(buf, 0, sizeof buf);
    buf[0] = 0;
    buf[1] = cmd;
    buf[2] = ++s_seq;
    buf[3] = plen;
    if (plen && payload) memcpy(buf + 4, payload, plen);
    buf[64] = crc8_frame(buf + 1, 63);

    DWORD written = 0;
    for (int retry = 0; retry < 3; retry++) {
        if (WriteFile(s_handle, buf, 65, &written, NULL) && written == 65)
            break;
        if (retry == 2) return false;
        Sleep(1);
    }

    if (resp) {
        uint8_t rbuf[65];
        memset(rbuf, 0, sizeof rbuf);
        rbuf[0] = 0;
        if (HidD_GetInputReport(s_handle, rbuf, 65)) {
            uint8_t *r = rbuf + 1;
            resp->ack_seq   = r[0];
            resp->status    = r[1];
            resp->buf_level = (uint16_t)(r[2] | (r[3] << 8));
            resp->tick      = (uint32_t)r[4] | ((uint32_t)r[5] << 8) |
                              ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
        } else {
            memset(resp, 0, sizeof *resp);
        }
    }
    return true;
}

bool rpfm_link_reset(void)
{
    rpfm_sid_stream_begin();
    uint8_t slot_mask = 0x01;
    return rpfm_link_send(CMD_RESET, &slot_mask, 1, NULL);
}

bool rpfm_link_bootsel(void)
{
    return rpfm_link_send(CMD_BOOTSEL, NULL, 0, NULL);
}

bool rpfm_set_phi2(int ntsc)
{
    uint8_t v = ntsc ? 1 : 0;
    return rpfm_link_send(CMD_PHI2, &v, 1, NULL);
}

bool rpfm_set_sid_step(int us)
{
    if (us < 1) us = 1;
    if (us > 20) us = 20;
    uint8_t v = (uint8_t)us;
    return rpfm_link_send(CMD_SID_STEP, &v, 1, NULL);
}

uint16_t rpfm_link_queue_level(void)
{
    if (s_handle == INVALID_HANDLE_VALUE) return 0;
    uint8_t rbuf[65];
    memset(rbuf, 0, sizeof rbuf);
    rbuf[0] = 0;
    if (!HidD_GetInputReport(s_handle, rbuf, 65)) return 0;
    return (uint16_t)(rbuf[3] | (rbuf[4] << 8));
}

/* ---- SID event stream ------------------------------------------------- */

void rpfm_sid_stream_begin(void)
{
    s_batch_n = 0;
}

bool rpfm_sid_push(uint16_t dt_us, uint8_t reg, uint8_t val)
{
    unsigned o = s_batch_n * 4;
    s_batch[s_batch_n * 4 + 0] = (uint8_t)(dt_us & 0xFF);
    s_batch[s_batch_n * 4 + 1] = (uint8_t)(dt_us >> 8);
    s_batch[s_batch_n * 4 + 2] = reg;
    s_batch[s_batch_n * 4 + 3] = val;
    (void)o;
    if (++s_batch_n == EV_PER_FRAME)
        return rpfm_sid_flush();
    return true;
}

bool rpfm_sid_flush(void)
{
    if (s_batch_n == 0) return true;
    unsigned len = s_batch_n * 4;
    bool ok = rpfm_link_send(CMD_SID_EVENTS, s_batch, (uint8_t)len, NULL);
    s_batch_n = 0;
    return ok;
}

bool rpfm_sid_write_now(uint8_t reg, uint8_t val)
{
    uint8_t p[2] = { reg, val };
    return rpfm_link_send(CMD_SID_WRITE, p, 2, NULL);
}

int64_t rpfm_now_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1000000.0 / (double)s_qpc_freq.QuadPart);
}
