#!/bin/bash
# flash.sh - 一键烧录 rpfm_sid_player，不需要按 BOOTSEL 按钮。
#
# 固件在 src/main.c 里处理 CMD_BOOTSEL (0x20) 时调 reset_usb_boot()，
# 所以主机发一帧 HID 就能让板子自己进烧录模式。
#
# 用法: bash scripts/flash.sh [uf2文件]
#   默认 build/rpfm_sid_player.uf2
#
# 盘符靠 RP2350 bootrom 必带的 INFO_UF2.TXT 认，不看卷标：
#   - 卷标可以是 "RP2350" / "RP2350A" / 用户改的名字，match '^RP' 会漏掉；
#   - 原来那版用 powershell 查盘符，一旦调用方把 PATH 收窄成只有 msys64 的
#     几个目录（构建脚本就是这么写的），powershell 就不在 PATH 里，再加上
#     2>/dev/null，于是每次都静默失败、明明挂着盘也报"没等到盘符"。
# 现在只用 bash 的 -f 判断，不依赖任何外部命令。
set -e
cd "$(dirname "$0")/.."

UF2="${1:-build/rpfm_sid_player.uf2}"
[ -f "$UF2" ] || { echo "找不到 UF2: $UF2"; exit 1; }
[ -f tools/rpfm_bootsel.exe ] || { echo "缺 tools/rpfm_bootsel.exe"; exit 1; }

echo "=== 固件: $UF2 ==="
md5sum "$UF2" 2>/dev/null || true

# 找一个带 INFO_UF2.TXT 的盘符。RP2350 bootrom 的盘里一定有这个文件，
# 普通 U 盘/移动硬盘没有，比按卷标猜可靠得多。
find_rp_drive() {
    local d
    for d in d e f g h i j k l m n o p q r s t u v w x y z; do
        [ -f "/$d/INFO_UF2.TXT" ] && { echo "$d"; return 0; }
    done
    return 1
}

DRIVE="$(find_rp_drive || true)"
if [ -z "$DRIVE" ]; then
    echo "[1/3] 发 BOOTSEL 指令..."
    tools/rpfm_bootsel.exe || echo "  (可能已经在 BOOTSEL 模式)"
    echo "[2/3] 等 RP2350 盘符..."
    for _ in $(seq 1 30); do
        DRIVE="$(find_rp_drive || true)"
        [ -n "$DRIVE" ] && break
        sleep 1
    done
else
    echo "[1/3] 板子已经在 BOOTSEL 模式（${DRIVE^^}:）"
fi
[ -n "$DRIVE" ] || {
    echo "30 秒内没等到 RP2350 盘符。确认 USB 线是数据线，或手动按住 BOOTSEL 再插。"
    exit 1
}

echo "[2/3] 目标盘 ${DRIVE^^}:"
echo "[3/3] 写入..."
cp "$UF2" "/$DRIVE/" || { echo "复制失败，检查盘是否还在 BOOTSEL 模式。"; exit 1; }
echo "完成，板子会自动重启。"
