/*
 * rpfm_bootsel.c - tell a running sid_rp2350 to reboot into BOOTSEL mode.
 *
 * The firmware handles CMD_BOOTSEL (0x20) by calling reset_usb_boot(0, 0),
 * so flashing never needs the physical button. This tool sends that one HID
 * frame and exits; the caller then waits for the RP2350 mass-storage drive.
 *
 * Build (from the rp2350 directory):
 *   gcc -O2 -o tools/rpfm_bootsel.exe tools/rpfm_bootsel.c -lhid -lsetupapi
 *
 * Exit codes: 0 = frame sent, 1 = device not found, 2 = send failed.
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

#define CMD_BOOTSEL 0x20
#define RPFM_FRAME_SIZE 64

static uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0x00;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static HANDLE find_device(void) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, i, &iface); i++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, NULL, 0, &req, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = malloc(req);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, detail, req, NULL, NULL)) {
            free(detail);
            continue;
        }

        HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        free(detail);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == VID && attr.ProductID == PID) {
            SetupDiDestroyDeviceInfoList(dev_info);
            return h;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return INVALID_HANDLE_VALUE;
}

int main(void) {
    HANDLE h = find_device();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "sid_rp2350 not found (VID=%04X PID=%04X).\n", VID, PID);
        fprintf(stderr, "Either it is already in BOOTSEL, or the firmware is not running.\n");
        return 1;
    }

    /* Report 0, then [cmd, seq, len, payload...], CRC over bytes 0..62. */
    uint8_t buf[RPFM_FRAME_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0;              /* report id */
    buf[1] = CMD_BOOTSEL;
    buf[2] = 0x01;           /* seq */
    buf[3] = 0x00;           /* len */
    buf[RPFM_FRAME_SIZE] = crc8(buf + 1, RPFM_FRAME_SIZE - 1);

    /* The device disappears mid-call, so a failure here is expected. */
    HidD_SetOutputReport(h, buf, sizeof(buf));
    CloseHandle(h);

    printf("BOOTSEL command sent.\n");
    return 0;
}
