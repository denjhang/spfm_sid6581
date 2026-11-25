#!/bin/bash
# flash.sh - 一键烧录 sid_rp2350，不需要按 BOOTSEL 按钮。
#
# 固件在 src/main.c 里处理 CMD_BOOTSEL (0x20) 时调 reset_usb_boot()，
# 所以主机发一帧 HID 就能让板子自己进烧录模式。
#
# 用法: bash scripts/flash.sh [uf2文件]
#   默认 build/sid_rp2350.uf2

set -e
cd "$(dirname "$0")/.."

UF2="${1:-build/sid_rp2350.uf2}"
[ -f "$UF2" ] || { echo "找不到 UF2: $UF2"; exit 1; }
[ -f tools/rpfm_bootsel.exe ] || { echo "缺 tools/rpfm_bootsel.exe"; exit 1; }

echo "=== 固件: $UF2 ==="

echo "[1/3] 发 BOOTSEL 指令..."
tools/rpfm_bootsel.exe || echo "  (可能已经在 BOOTSEL 模式)"

echo "[2/3] 等 RP2350 盘符..."
DRIVE=""
for i in $(seq 1 30); do
  DRIVE=$(powershell -NoProfile -Command "(Get-Volume | Where-Object { \$_.FileSystemLabel -match '^RP' }).DriveLetter" 2>/dev/null | tr -d '\r')
  [ -n "$DRIVE" ] && { echo "  找到 ${DRIVE}:"; break; }
  sleep 1
done
[ -n "$DRIVE" ] || { echo "30 秒内没等到盘符。手动按住 BOOTSEL 再插 USB。"; exit 1; }

echo "[3/3] 写入..."
cp "$UF2" "${DRIVE}:/"
echo "完成，板子会自动重启。"
