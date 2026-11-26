# rp2350 —— SID 6581 控制器固件（RP2350A）

> 2026-11-26。**实时播放已达标**：双核 + 环形队列 + 水位线反压之后，
> Turbo Outrun（4500 写/秒）连续播完全程不掉音、不拖慢。
>
> 同日修复三处时间轴缺陷（见"踩过的坑" 11–13）：播放列表第 2 首起开头赶拍子、
> 队列见底后 digi 鼓退化成噪音、预填按条数算导致稀疏开头预填不足。

这是 SPFM 方案的替代品。SPFM 协议的帧内时序上限（见 [../docs/SPFM时序精度上限.md](../docs/SPFM时序精度上限.md)）
由 RP2350A 自己生成总线时序解决——主机只送带微秒时间戳的事件，抖动到不了芯片。

## 现状

| 项 | 状态 |
|---|---|
| RP2350A → 2× 74LVC245 → 374/02/32 → SID 6581 | ✅ 出声 |
| PHI2 由 PIO 生成 985 248 Hz（PAL）/ 1 022 727（NTSC） | ✅ 已验证 |
| 自动烧录（HID `CMD_BOOTSEL`） | ✅ 每次都用，不用按按钮 |
| **`.sidvgm` / `.sid` 实时播放（`rpfm_sid.exe`）** | ✅ **4500 写/秒不掉音、不拖慢** |
| 双核：Core 1 跑总线，Core 0 只剩 USB | ✅ |
| 水位线反压（队列 >6000 条就等） | ✅ |
| 换曲边界 `CMD_SID_STOP`：队列丢弃 + 时钟重锚 | ✅ 2026-11-26 |
| 队列见底自动重锚（digi 稀疏开头不再退化成噪音） | ✅ 2026-11-26 |
| 4 颗 WS2812 写入密度表 | ✅ 亮度已确认 |
| `.sidreg` 事件流播放（`tools/sid_stream.exe`） | ✅ 早期工具，保留 |

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

主机侧播放器（在仓库根目录跑，复用 `build/*.o`）：

```bash
bash rp2350/scripts/build_host.sh     # -> build/rpfm_sid.exe
```

依赖：ARM GCC 16.2 + picotool 2.3.1（在 `/d/msys64/ucrt64`），Pico SDK 2.2.0 已随仓库带在 `pico-sdk-2.2.0/`。

## 自动烧录

```powershell
cd E:\working\zcode-projects\spfm_sid6581\rp2350
.\tools\rpfm_bootsel.exe     # 让板子自己进 BOOTSEL
# 等 G: 盘出现，然后
Copy-Item .\build\rpfm_sid_player.uf2 G:\
```

固件里 `CMD_BOOTSEL (0x20)` 调 `reset_usb_boot()`，所以永远不用按按钮。

## 播放 .sidvgm / .sid

`tools\sid_stream.exe` 是上一轮的临时工具（只认 `.sidreg`）。正式播放器是
`build\rpfm_sid.exe`，它是 `..\src\spfmplayer.c` 换传输层改出来的：
播放列表、键盘、libsidplayfp 录制、`.sidvgm`/`.sidreg` 回放都沿用，
只把 SPFM 总线换成 RPFM HID 帧。

```powershell
cd E:\working\zcode-projects\spfm_sid6581
.\build\rpfm_sid.exe sidvgm\turboutrun              # 整个文件夹连播
.\build\rpfm_sid.exe "sidvgm\atmos.sidvgm" --seconds 60
.\build\rpfm_sid.exe sid\games                      # .sid 直接用 libsidplayfp 跑
.\build\rpfm_sid.exe sidvgm\turboutrun --step 10    # 扫总线步进时间
```

关键部分：

- **`.sid` 走 libsidplayfp**（它自己的 6502/VIC/CIA），不是内置 fake6502。
  CIA 定时器和 `$D418` 数字鼓在帧内写寄存器，只有 libsidplayfp 会如实报告。
- **时钟由曲子的 region 决定**：板子收到 `CMD_PHI2` 后切 985248 / 1022727 Hz。
- **时间戳在事件里**：主机把每个写连同 µs 增量一起推给板子，板子按绝对
  deadline 执行。所以 USB 抖动不影响发声时刻。
- `--pad` / `--digi` / `--proto` 全部**不存在**——它们是为 1.5 Mbaud UART 补时序的，
  这里每个事件自带微秒时间戳，没有线速问题。
- **`--step US`**（1–20，默认 20）改总线每步时间。一次写 7 步，所以 20 µs
  = 140 µs/写。这个默认值是从 YM2413 抄的保守值，不是 SID 实测值，压下去
  写密度上限直接提高。

## 板载 WS2812：写入密度表

4 颗灯 = 3 个声部 + `$D418`，**颜色固定**，亮度反映写入密度：

| 灯 | 声道 | 颜色 |
|---|---|---|
| 0 | 声部 1（`$D400-$D406`） | 绿 |
| 1 | 声部 2（`$D407-$D40D`） | 蓝 |
| 2 | 声部 3（`$D40E-$D414`） | 黄 |
| 3 | `$D415-$D418`（滤波 + 音量） | 红 |

这就是 RPFM 原版的分配（`src/rpfm/main.c:42`：`0x006600 0x000066 0x666600 0x660000`）。
那四个字面量读作 `0x00GGRRBB`——PIO 是左移 MSB-first、24 位阈值，字推的是
`value << 8`，所以**线上第一个字节是 bits 23:16，WS2812 要绿先出**。当成
`0xRRGGBB` 读，红绿互换，颜色就全错。颜色一律走 `urgb_u32()`。

亮度 = 这一个 20 ms tick 内该声道收到的写入数，6 次写算满亮：

```c
uint32_t lvl = s_ws_hits[i] * LED_MAX / LED_FULL_HITS;   // LED_MAX = 0x10
```

**没有累加**，所以不会有任何东西往上爬；也不做颜色混合，所以没有灯会偏白。
`ws_frame()` 是唯一把 level 变成颜色的地方，它对每个通道都夹一遍 `LED_MAX`，
不存在绕过上限的路径。

RPFM 原版其实**完全没有亮度控制**：`ws_led_on()` 直接把 `cs_colors[slot]`
原样写进去，再挂一个 50 ms 定时器，超时 `ws_led_off_all()`。这里是它唯一
的中间态——同样的固定色，按写入密度调暗。

三条约束，别自己发明：

1. **颜色必须走 `urgb_u32()`**（GRB 顺序）。
2. **上限 `LED_MAX = 0x10`**，`ws_frame()` 里逐通道夹住。`led_test.c` 用的
   0x18 是"同时只有一两颗亮"时的值；这里四颗会一起亮，所以更保守。0x18
   在四颗全亮时偏亮，0x28 以上刺眼。
3. **`ws2812_update()` 发完 4 个字就返回，什么都不等。** 帧距靠
   `led_update()` 的 20 ms 节拍保证——那是 280 µs latch 时间的 60 倍。
   往里面塞 `busy_wait_us_32()` 会卡住主循环，而主循环还要喂 `tud_task()`
   和 SID 事件队列。

**灯和播放现在在不同核上，这条不再需要了。** 分核之前，播放的 deadline 等待
就在 Core 0 主循环里，那个 `while` 必须顺带调 `led_update()`，否则灯会停在
第一批事件的画面上。现在 `write_reg_sid()` 在 Core 1，`led_update()` 在
Core 0，互不影响。

灯的刷新在 `led_update()` 里 20 ms 一拍合成，**不在 `write_reg_sid()` 里**——
后者峰值 4500 次/秒，而一次 `ws2812_update()` 要占 PIO 约 120 µs。
`write_reg_sid()` 只把计数加到 `s_ws_hits[]`（Core 1 写、Core 0 读，所以是
`volatile`）。

## 上电行为

**不发声。** 早期版本上电会自检琶音，已删除。板子只清寄存器、
`$D418` 停在 0，等主机推事件。

## 协议

64 字节 HID 帧，`[CMD, SEQ, LEN, payload..., CRC8]`，CRC 多项式 0x31。

| CMD | 内容 |
|---|---|
| `0x30` | `CMD_SID_EVENTS` `[dt_lo,dt_hi,addr,data] × N` 带时间戳；`addr=0xFF` = 只等待不写 |
| `0x32` | `CMD_SID_WRITE` `[addr,data] × N` 立即写 |
| `0x31` | `CMD_PHI2` `[0=PAL/1=NTSC]` |
| `0x33` | `CMD_SID_STEP` `[1..32]` 数据相保持几个 PHI2 周期（默认 12） |
| `0x34` | `CMD_SID_START` 释放播放闸门 + 以"现在"为新原点重新锚定时钟（预填完成后发） |
| `0x35` | `CMD_SID_STOP` 关闸门、丢掉队列里剩下的、忘掉时间轴。**换曲边界用它** |
| `0x03` | `CMD_RESET` 清事件队列 + 复位 IC#（100 ms 脉冲，只在开程序时用一次） |
| `0x20` | `CMD_BOOTSEL` |

## 源码结构

固件从 RPFM（`E:\working\vscode-projects\YM2163-Midi\RPFM`）原样复制后小改。
共 16 个文件 1688 行，**只动了三处**：

| 文件 | 改动 |
|---|---|
| `src/spfm_bus.pio` | 13 位 → 11 位（A1/A2 不用）；`autopull` 关掉 |
| `src/pio_cs.pio` | `autopull` 关掉 |
| `src/main.c` | 引脚号（CS 12 / DIR 11 / PHI2 14 / WS2812 16）、`phi2_init()`、`write_reg_sid()`、SID 事件队列与 Core 1 播放循环、WS2812 密度表 |

`src/phi2.pio` 是新增的，抄自 USBSID-Pico 的 `src/pio/clock.pio`（用 `set pins` 而不是 sideset）。
`src/ring_buf.h`、`src/command_buffer.h`、`src/vgm_player.h` 与 RPFM 逐字节相同。

主机侧：

| 文件 | 来源 |
|---|---|
| `tools/rpfm_link.[ch]` | `RPFM_Player/src/rpfm_hid.cpp` 改 C，增加 SID 事件批处理 |
| `tools/rpfm_sid.c` | `../src/spfmplayer.c` 换传输层（骨架、播放列表、键盘、libsidplayfp 录制全部沿用） |
| `scripts/build_host.sh` | 复用仓库 `build/*.o` 与 `third_party/local/lib` 静态库 |

### 诊断小工具（`rp2350/tools/`，都不发声、不改固件）

计时类问题不能靠猜，这几个工具各自只量一个数：

| 工具 | 量什么 | 用法 |
|---|---|---|
| `hidbench.c` | 主机这一端每秒能推多少 64 字节帧 | `hidbench.exe 1` → 本机 6846 帧/s = 10.2 万事件/s |
| `wbench.c` | 板子连续写寄存器的极限 | `wbench.exe 4000` → 18.9 µs/写 = 5.3 万写/s |
| `qprobe.c` | 固件上报的队列水位是不是真的 | `qprobe.exe`（推 2000 条应报 2000，推到 1.2 万应饱和在 8191） |
| `drainprobe.c` | 一段真实事件的实际播放速率 vs 文件本身 | `drainprobe.exe <file.sidvgm> <起始ms> <条数>` |
| `pairprobe.c` | 播放列表里"第 2 首"的开头是否赶拍子 | `pairprobe.exe <A.sidvgm> <B.sidvgm> 1200` |
| `gapsprobe.c` | 静音后接密集段，鼓的间距是否保住 | `gapsprobe.exe` |
| `liveprobe.c` | 走 libsidplayfp 实时路径时队列深度曲线 | `liveprobe.exe <file.sid> 0 12` |
| `lsfpdump.c` | 把实时 `.sid` 的事件流导成文本，和 `.sidvgm` 逐条 diff | `lsfpdump.exe <file.sid> 0 6` |

一次编译全部（先跑过 `scripts/build_host.sh`，它提供 `.o` 和静态库）：

```bash
bash rp2350/scripts/build_probes.sh
```

## 踩过的坑

**完整清单见 [../docs/踩坑记录_RP2350移植.md](../docs/踩坑记录_RP2350移植.md)**，含代码、症状和成因。

摘要：

1. **同一组引脚只能有一个 PIO 状态机。** RPFM 原文会在总线 SM 之外再启动一个 AY8910 SM，
   同一个程序抢同一组 GPIO0–10，A0 被盖成 0，整个芯片像死了一样。已删除。
2. **`out pins, N` 配 autopull 时阈值必须等于 N。** 否则和显式的 `pull block` 打架，
   每两个推送的字丢一个。现在 `autopull=false`。
3. **`pio_sm_put_blocking` 打向未使能的 SM 会永久阻塞**，卡死在 `main()`，
   USB 从不枚举 → BOOTSEL 发不进去。WS2812 输出前有 `s_ws_ready` 闸门。
4. **播放循环所在的那一核必须定期跑 `tud_task()`**，否则 USB 协议栈停摆。
   现在播放搬到了 Core 1，Core 0 专职跑 `tud_task()`，这个约束自然满足。
5. **PHI2 用 `set pins` 不用 sideset。** 抄 USBSID-Pico 的 `clock.pio`。
6. **单核跑播放会拖慢。** 4500 写/秒 × 140 µs = 单核超过 60%，密集帧 100%+，
   必然饿死 `tud_task()`。分核之后才实时——这是最后才找对的一条。
7. **队列满了是静默丢事件。** `sid_q_push()` 返回 false 但没人看，听起来就是
   "拖慢 + 掉音"。水位线必须当反压用。
8. **`wait` 不是"等边沿"。** 写成 `wait 1 pin` → `wait 0 pin` 时，若 SM 到达
   时 PHI2 已经高，第一句直接穿过，第二句也几乎立刻穿，两个字之间只剩几十纳秒。
   必须 `wait 0` → `out` → `wait 1`（USBSID 的顺序），才能保证每字至少一个周期。
   这一条把 digi 鼓从噪音救回来了。
9. **`/CS` 窗口不能跟着节拍一起缩。** 节拍变快是对的（1 周期/字），但数据相
   必须保持 N 个周期（默认 12），否则密集的 `$D418` 写会有一批锁存失败。
   两件事必须分开调。
10. **没有预填就开始播 = 开头赶拍子。** RPFM 的 Phase 1 先填 1/4 buffer 再发
    START，Phase 2 才边推边播。不做这一步，头几百个事件按 USB 到达速度执行。
11. **`CMD_SID_START` 必须重开时间轴，否则只有第一首是对的。** 固件的 deadline
    是绝对时间；第二首预填的事件在"上一首的 deadline"之后全是过期事件，于是
    整段预填按总线速度灌出去。用 `build/pairprobe.exe` 量过：同一首开头 999 ms
    的音乐，第一首用 1101 ms 播完，接着播第二首只用了 439 ms —— 这就是"只有
    `turboutrun_s01` 这种单曲很完美、`sidvgm` 全目录开头赶拍子"的全部原因。
12. **换曲要用 `CMD_SID_STOP (0x35)`，不能用 `CMD_RESET`。** RESET 是 100 ms 的
    IC# 脉冲，每首之间来一次会卡；STOP 只关闸门 + 丢队列 + 重锚时钟。
    主机的 `sched_reset()` 在预填之前发它，并补一遍清寄存器（播放列表原本发的
    那次清寄存器会被 STOP 一起丢掉）。
13. **队列空的时候不能让时钟继续跑。** 一首曲子里可以静音几秒再突然来一段
    密集 `$D418`（Giana RSID 第一秒 79 次写，之后三秒几乎不写，然后每秒 7247
    次）。队列空着时 deadline 追不上墙钟，等那批事件到了全都"过期"，板子就按
    总线速度（15 条/285 µs）灌，而不是 137 µs 一条 —— 听感就是鼓声退化成噪音。
    所以 `sid_q_pop()` 失败且墙钟已超过 deadline 20 ms 时，把 deadline 重锚到
    "现在"。间距（音符本身）完全不变，只是原点移动，而那段是静音。

## 双核与流控

RPFM 高性能的三个部件在这里都用上了，结构照 `src/rpfm/main.c`：

| 部件 | RPFM | 这里 |
|---|---|---|
| 环形缓冲 | `cmd_buf_t`（64 KB，VGM 字节） | `sid_ev_t` 队列（8192 条 = 32 KB） |
| 水位线 | `STATUS_BUF_HIGH`（>75% 填满） | 同一个位，用在 `get_report_cb` |
| 双核 | Core 0 = USB，Core 1 = VGM 播放 | Core 0 = USB + 灯，Core 1 = SID 总线 |

**为什么必须双核**：`write_reg_sid()` 一次 7 步 × 20 µs = **140 µs**，
Turbo Outrun 是 733634 写 / 162.6 s ≈ **4500 写/秒**，也就是单核 63%，
密集帧远超 100%。放在 Core 0 上必然饿死 `tud_task()` —— 这就是"拖慢"的
来源。Core 1 本来空闲，把播放循环搬过去之后 Core 0 只剩 USB，和 RPFM
保 FM 播放流畅的做法完全一样。

**水位线是反压，不只是标志位。** 阈值 `Q_HIGH 4096` / `Q_LOW 2048`，
就是 RPFM 的"buffer > 50% 就等，排空到 25% 再继续"。没有这一层的话队列满了
`sid_q_push()` 会直接返回 false，事件**静默丢失**——听起来就是"拖慢 + 掉音"。

**预填 + START 闸门，照 RPFM 的 Phase 1 / Phase 2**
（`RPFM_Player/src/ay8910_window.cpp`）：

```
Phase 1: 推满 bufTargetBytes/4，此时设备还不消费
         然后发 CMD_VGM_START
Phase 2: 继续推，buffer > 50% 就等
```

这里对应的是 `CMD_SID_START (0x34)`：固件有 `s_sid_running` 闸门，
没收到 START 前 Core 1 一个事件都不消费；主机先预填，再发 START，
固件在这条命令里把时间轴原点重锚到"现在"。

预填的判据是**音乐时间 + 事件数两个都要满足**（`prefill_more()`：
≥800 ms 且 ≥1500 条，上限 6000 条 / 6 s）。只用事件数不够：Giana RSID
第一秒只写 79 次，1500 条要到 4 秒以后，那时它的 digi 段正好开始，队列
只剩 39 条——`build/liveprobe.exe` 实测。队列中途见底 = 后面那段鼓按总线
速度播。两个都满足时，4 秒的 6502 模拟只花约 0.25 秒主机时间，而且板子的
闸门还关着，听众不会等。

**没有这个预填，第一个 audio 块就是"按 USB 到达速度执行"**——听感就是开头
一两秒赶拍子。这是 RPFM 早就解决的问题。

单写者约定照 RPFM 的 `ring_buf.h`：head 只在 Core 0 写，tail 只在 Core 1 写，
所以不需要锁。`CMD_RESET` / `CMD_SID_STOP` 也因此只置一个请求位（外加一个
队列截断位置），由 Core 1 自己清队列。

## 时序参数

**总线是 PHI2 时钟驱动的，没有主机延时。** 抄 USBSID-Pico 的
`src/pio/bus_control.pio`。

```asm
.wrap_target
    pull block
    wait 0 pin 0     ; 先确认在时钟低位（若为高就阻塞）
    out pins, 11     ; 在低电平期间换总线
    wait 1 pin 0     ; 真的等上升沿
.wrap
```

`wait` 不是"等边沿"，是"电平符合就通过"。写成 `wait 1` → `wait 0` 会让两个字
之间只剩几十纳秒，`/CS` 窗口不够锁存——这就是 digi 鼓退化成噪音的根因。
先等低、再等高，才能保证每个字至少占一个完整 PHI2 周期。

**`/CS` 窗口 = `SID_HOLD` 个 PHI2 周期**，默认 **12**（PAL 12.2 µs）。
做法是把数据相那个字连推 N 次，PIO 每周期才换一次总线，所以低电平时长正好
是 N 个时钟周期——等价于 USBSID 的 `delay_timer.pio` 周期计数器。
`CMD_SID_STEP (0x33)` 可改，范围 1..32。

一次寄存器写 = 3 个地址字 + 12 个数据字 + 2 + 1 个 idle ≈ **18 µs**，
原来 `busy_wait_us_32(20)` 的版本是 140 µs。digi 鼓（4500 写/秒）已上机确认正常。
