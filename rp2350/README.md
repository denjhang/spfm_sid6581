# rp2350 —— SID 6581 控制器固件（RP2350A）

> 2026-11-25。**里程碑：RP2350A 直接驱动 SID 6581 出声成功。**

这是 SPFM 方案的替代品。SPFM 协议的帧内时序上限（见 [../docs/SPFM时序精度上限.md](../docs/SPFM时序精度上限.md)）
由 RP2350A 自己生成总线时序解决——主机只送带微秒时间戳的事件，抖动到不了芯片。

## 现状

| 项 | 状态 |
|---|---|
| RP2350A → 2× 74LVC245 → 374/02/32 → SID 6581 | ✅ **出声**（四音琶音已确认） |
| PHI2 由 PIO 生成 985 248 Hz（PAL） | ✅ 已验证 |
| 自动烧录（HID `CMD_BOOTSEL`） | ✅ 每次都用，不用按按钮 |
| 上电自检（密集琶音+和弦+噪声鼓） | ✅ |
| `.sidreg` 事件流播放（`tools/sid_stream.exe`） | ✅ 已跑通 8079 事件 / 10 秒 |
| 完整 `.sid` 播放 | ⬜ 下一步 |

## 硬件

| 信号 | GPIO | 去向 |
|---|---|---|
| D0–D7 | 0–7 | U1 74LVC245 A1–A8 |
| WR# | 8 | U2 → SID pin7 (R/W) |
| /RD | 9 | U2（预留） |
| A0 | 10 | U2 → 374 `LE` 逻辑 |
| DIR | 11 | U1 pin1（1 = 写） |
| CS0# | 12 | U2 → U5 pin5 |
| PHI2 | 14 | SID pin8（**PIO 输出**，不用晶振） |
| WS2812 | 16 | 板载（不可动） |
| IC# | 21 | SID pin5 |
| LED | 25 | 板载（不可动） |

完整接线见 [../docs/接线表_完整_RP2350A.md](../docs/接线表_完整_RP2350A.md)。

## 构建

```bash
export PATH=/d/msys64/ucrt64/bin:/d/msys64/mingw64/bin:/d/msys64/usr/bin:/usr/bin:/bin
cd E:/working/zcode-projects/spfm_sid6581/rp2350
cmake -S . -B build -G "Unix Makefiles" \
      -DCMAKE_MAKE_PROGRAM=/d/msys64/usr/bin/make.exe \
      -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=pico2
cmake --build build -j8
```

产物：`build/rpfm_sid_player.uf2`（SID 播放器）、`build/led_test.uf2`（流水灯自检）。

依赖：ARM GCC 16.2 + picotool 2.3.1（在 `/d/msys64/ucrt64`），Pico SDK 2.2.0 已随仓库带在 `pico-sdk-2.2.0/`。

## 自动烧录

```powershell
cd E:\working\zcode-projects\spfm_sid6581\rp2350
.\tools\rpfm_bootsel.exe     # 让板子自己进 BOOTSEL
# 等 G: 盘出现，然后
Copy-Item .\build\rpfm_sid_player.uf2 G:\
```

固件里 `CMD_BOOTSEL (0x20)` 调 `reset_usb_boot()`，所以永远不用按按钮。

## 播放 .sidreg

```powershell
.\tools\sid_stream.exe ..\build\cmp\R-Type_nat.sidreg
```

`.sidreg` 是本仓库 `sidextract.exe` 输出的文本格式（`时间ms 寄存器hex 值hex`），
工具把它转成 µs 增量、按 `CMD_SID_EVENTS (0x30)` 每帧 15 条推给板子。
板子按绝对 deadline 执行，所以 USB 抖动不影响时序。

## 协议

64 字节 HID 帧，`[CMD, SEQ, LEN, payload..., CRC8]`，CRC 多项式 0x31。

| CMD | 内容 |
|---|---|
| `0x30` | `CMD_SID_EVENTS` `[dt_lo,dt_hi,addr,data] × N` 带时间戳 |
| `0x32` | `CMD_SID_WRITE` `[addr,data] × N` 立即写 |
| `0x31` | `CMD_PHI2` `[0=PAL/1=NTSC]` |
| `0x03` | `CMD_RESET` 复位 IC# 并清寄存器 |
| `0x20` | `CMD_BOOTSEL` |

## 源码结构

从 RPFM（`E:\working\vscode-projects\YM2163-Midi\RPFM`）原样复制后小改。
共 16 个文件 1688 行，**只动了三处**：

| 文件 | 改动 |
|---|---|
| `src/spfm_bus.pio` | 13 位 → 11 位（A1/A2 不用）；`autopull` 关掉 |
| `src/pio_cs.pio` | `autopull` 关掉 |
| `src/main.c` | 引脚号（CS 12 / DIR 11 / PHI2 14 / WS2812 16）、`phi2_init()`、`write_reg_sid()`、琶音自检、SID 事件队列 |

`src/phi2.pio` 是新增的，抄自 USBSID-Pico 的 `src/pio/clock.pio`（用 `set pins` 而不是 sideset）。

## 踩过的坑

**完整清单见 [../docs/踩坑记录_RP2350移植.md](../docs/踩坑记录_RP2350移植.md)**，含代码、症状和成因。

摘要：

1. **同一组引脚只能有一个 PIO 状态机。** RPFM 原文会在总线 SM 之外再启动一个 AY8910 SM，
   同一个程序抢同一组 GPIO0–10，A0 被盖成 0，整个芯片像死了一样。已删除。
2. **`out pins, N` 配 autopull 时阈值必须等于 N。** 否则和显式的 `pull block` 打架，
   每两个推送的字丢一个。现在 `autopull=false`。
3. **`pio_sm_put_blocking` 打向未使能的 SM 会永久阻塞**，卡死在 `main()`，
   USB 从不枚举 → BOOTSEL 发不进去。WS2812 输出前有 `s_ws_ready` 闸门。
4. **播放循环里必须调 `tud_task()`**，否则 USB 协议栈停摆。
5. **PHI2 用 `set pins` 不用 sideset。** 抄 USBSID-Pico 的 `clock.pio`。

## 时序参数

`write_reg_sid()` 一次写 7 个 PIO 字（6 步 + 1 个 idle），每步 `SID_STEP_US = 20` µs，
合计约 140 µs/次寄存器写。R-Type 峰值约 800 次/秒，占 11%。

20 µs 是从 `ym2413_pio_write_reg()` 抄的保守值；知道 autopull 那个 bug 之后，1 µs
可能也能用，但没再扫——能出声优先。
