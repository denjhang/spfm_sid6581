/*
 * sid_stream.c - feed a .sidreg event file to rpfm_sid_player over USB HID.
 *
 * .sidreg is the text format our own sidextract.exe writes:
 *
 *     # comments
 *     <time_ms_double> <reg_hex> <val_hex>
 *
 * We convert the absolute millisecond stamps into microsecond deltas and pack
 * them into CMD_SID_EVENTS (0x30) frames: [dt_lo,dt_hi,addr,data]. The player
 * applies each event against an absolute deadline, so USB jitter never reaches
 * the SID.
 *
 * Build:
 *   gcc -O2 -o tools/sid_stream.exe tools/sid_stream.c -lhid -lsetupapi
 *
 * Usage:
 *   tools\sid_stream.exe <file.sidreg> [--loop] [--speed 1.0]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#define VID 0x2E8A
#define PID 0x1090

#define RPFM_FRAME_SIZE  64
#define CMD_SID_EVENTS   0x30
#define CMD_BOOTSEL      0x20
#define CMD_RESET        0x03

static uint8_t crc8(const uint8_t *d, int len) {
    uint8_t c = 0;
    for (int i = 0; i < len; i++) {
        c ^= d[i];
        for (int j = 0; j < 8; j++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

static HANDLE find_device(void) {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsA(&g, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

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
        if (HidD_GetAttributes(h, &a) && a.VendorID == VID && a.ProductID == PID) {
            SetupDiDestroyDeviceInfoList(di);
            return h;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(di);
    return INVALID_HANDLE_VALUE;
}

static BOOL send_frame(HANDLE h, uint8_t cmd, uint8_t seq,
                       const uint8_t *payload, uint8_t plen) {
    uint8_t buf[RPFM_FRAME_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0;                 /* report id */
    buf[1] = cmd;
    buf[2] = seq;
    buf[3] = plen;
    if (plen && payload) memcpy(buf + 4, payload, plen);
    buf[RPFM_FRAME_SIZE] = crc8(buf + 1, RPFM_FRAME_SIZE - 1);
    return HidD_SetOutputReport(h, buf, sizeof(buf));
}

typedef struct { double t_ms; uint8_t reg, val; } ev_t;

static int load_sidreg(const char *path, ev_t **out, size_t *n_out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t n = 0, cap = 0;
    ev_t *v = NULL;
    char line[256];

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;

        double t; unsigned r, d;
        if (sscanf(p, "%lf %x %x", &t, &r, &d) == 3) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8192;
                ev_t *nv = realloc(v, cap * sizeof(*nv));
                if (!nv) { free(v); fclose(f); return -1; }
                v = nv;
            }
            v[n].t_ms = t;
            v[n].reg  = (uint8_t)(r & 0x1F);
            v[n].val  = (uint8_t)d;
            n++;
        }
    }
    fclose(f);
    *out = v;
    *n_out = n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.sidreg> [--loop] [--speed N]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int loop = 0;
    double speed = 1.0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--loop")) loop = 1;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atof(argv[++i]);
    }

    ev_t *ev; size_t n;
    if (load_sidreg(path, &ev, &n) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    printf("%s: %zu events, %.1f s\n", path, n, n ? ev[n - 1].t_ms / 1000.0 : 0.0);

    HANDLE h = find_device();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "rpfm_sid_player not found (VID=%04X PID=%04X)\n", VID, PID);
        free(ev);
        return 1;
    }

    uint8_t seq = 0;
    send_frame(h, CMD_RESET, seq++, NULL, 0);
    Sleep(50);

    const uint8_t per_frame = 15;   /* 15 * 4 bytes = 60, one frame */
    uint8_t payload[60];

    do {
        double prev_ms = 0.0;

        for (size_t i = 0; i < n; ) {
            uint8_t cnt = 0;

            while (cnt < per_frame && i < n) {
                double dt_ms = ev[i].t_ms - prev_ms;
                if (dt_ms < 0) dt_ms = 0;
                dt_ms /= speed;

                uint32_t dt_us = (uint32_t)(dt_ms * 1000.0 + 0.5);
                if (dt_us > 65535) {
                    /* A gap longer than 65 ms: split it into a silent filler
                     * event pair so the delta field stays 16-bit. */
                    payload[cnt * 4 + 0] = 0xFF;
                    payload[cnt * 4 + 1] = 0xFF;
                    payload[cnt * 4 + 2] = 0x18;   /* volume */
                    payload[cnt * 4 + 3] = 0x0F;   /* (harmless no-op write) */
                    cnt++;
                    prev_ms += 65.535 * speed;
                    continue;
                }

                payload[cnt * 4 + 0] = (uint8_t)(dt_us & 0xFF);
                payload[cnt * 4 + 1] = (uint8_t)((dt_us >> 8) & 0xFF);
                payload[cnt * 4 + 2] = ev[i].reg;
                payload[cnt * 4 + 3] = ev[i].val;
                cnt++;
                prev_ms = ev[i].t_ms;
                i++;
            }

            if (!send_frame(h, CMD_SID_EVENTS, seq++, payload, (uint8_t)(cnt * 4))) {
                fprintf(stderr, "HID write failed at event %zu\n", i);
                CloseHandle(h);
                free(ev);
                return 1;
            }
        }
    } while (loop);

    /* Let the device drain whatever is still queued before we exit. */
    Sleep(300);

    printf("done\n");
    CloseHandle(h);
    free(ev);
    return 0;
}
