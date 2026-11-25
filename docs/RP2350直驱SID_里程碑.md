# RP2350A 直驱 SID 成功 —— 里程碑记录

> 2026-11-25。RP2350A 绕过 SPFM，自己生成总线时序驱动 SID 6581，出声成功。

## 为什么有这一步

SPFM 协议只给主机三种动作：Path A（4 字节）、Path B（3 字节）、NOP。
`/WR`、`/CS`、`A0` 全由 STM32 固件在收到包之后自己产生，**主机唯一的时间刻度是
6.667 µs（一个线字节的 NOP）或 `Sleep` 的 ~1 ms**。普通 PSID 的帧内微秒级抖动
因此是采样定理问题，无解——这是 [SPFM时序精度上限.md](SPFM时序精度上限.md) 的结论，
也是 §0.3 第 3 项"换控制器让 SID 脱离 SPFM"的由来。

现在这条路走通了。

## 硬件

```
RP2350A (3.3V) → U1/U2 74LVC245 → U3 74LS374 + U4 74HCT02 + U5 74HCT32 → SID 6581
```

SID 那侧的逻辑和 SPFM 时代**一个字没改**（`LE = NOR(A0,/WR)`、`TRIG_n = ¬A0∨/WR∨CSn`）。
变的是入口：从 SPFM 排针换成两颗 245，PHI2 从晶振换成 PIO。

接线见 [接线表_完整_RP2350A.md](接线表_完整_RP2350A.md)。

**PHI2 现在由 PIO 生成 985 248 Hz（PAL），不需要晶振。** 分频
150 MHz/(2×985248)=76.123，PIO 的 16.8 位小数分频落在 985 272 Hz，
误差 +24.6 ppm（0.04 音分）。用 libsidplayfp 的主时钟而不是 1 MHz，
是为了让 PAL 曲子不再高 25.7 音分。

## 软件

固件在 [../rp2350/](../rp2350/)，从 RPFM 原样复制后只改了三处：

| 文件 | 改动 |
|---|---|
| `src/spfm_bus.pio` | 13 位 → 11 位；`autopull` 关掉 |
| `src/pio_cs.pio` | `autopull` 关掉 |
| `src/main.c` | 引脚号、`phi2_init()`、`write_reg_sid()`、琶音自检、SID 事件队列 |
| `src/phi2.pio` | 新增，抄自 USBSID-Pico 的 `clock.pio` |

主机侧只需要送带微秒时间戳的事件（`CMD_SID_EVENTS 0x30`），
板子按绝对 deadline 执行。**USB 抖动到不了 SID**，这正是换掉 SPFM 的目的。

`tools/sid_stream.exe` 把 `.sidreg`（本仓库 `sidextract.exe` 的输出格式）
转成事件流推给板子，已跑通 R-Type 8079 事件 / 10 秒。

## 三个必须记住的坑

**1. 同一组引脚只能有一个 PIO 状态机。**
RPFM 原文的 `pio_bus_init()` 在启动总线 SM 之后，还会调 `ay8910_sm_init()`
——同一个 `spfm_bus_write` 程序、同一组 GPIO0–10、只是分频不同。
两个 SM 都在 `out pins`，互相覆盖输出，**A0 被另一路盖成 0**，
万用表量一直是低电平，整个芯片像死了一样。SID 不用 AY，删掉即好。

**2. `out pins, N` 配 autopull 时阈值必须等于 N。**
程序体里是显式 `pull block`，配置里又是 `out_shift(..., autopull=true, 32)`，
而每条 `out pins, 11` 只吃 11 位。每次 out 之后 autopull 立刻从 FIFO 补一个字进
OSR，下一轮 `pull block` 又把它覆盖——**每两个推送的字丢一个**，地址和数据错位。
正确做法：`autopull=false`，只靠 `pull block` 取数。

**3. `nop side 1` / `nop side 0` 那套 sideset 写法在本板上不出时钟。**
改用 USBSID-Pico 的 `set pins, 1` / `set pins, 0`，引脚状态由 SM 自己的
输出寄存器锁存，不依赖 sideset 编码。生成的机器码就是 `0xe001 / 0xe000`。

## 时序参数

`write_reg_sid()` 一次写 7 个 PIO 字（6 步 + 1 个 idle 字），
每步 `SID_STEP_US = 20` µs，合计约 140 µs/次寄存器写。

20 µs 是从 `ym2413_pio_write_reg()` 抄的保守值。知道 autopull 那个 bug 之后
1 µs 可能也能用，但没再扫——能出声优先。R-Type 峰值 800 次/秒，占 11%，够用。

## 和 SPFM 方案的对比

| 项目 | SPFM 挂 SID | RP2350A 直驱 |
|---|---|---|
| 主机时间刻度 | 6.667 µs（NOP） | µs 级事件时间戳 |
| 帧内抖动 | 不可控（协议上限） | 由板子自产，USB 不进时序 |
| 一次寄存器写 | 26.67 µs（4 字节线速） | 140 µs（可压到更低） |
| 带宽瓶颈 | FT232R 的 FT_Write 次数 | 无（USB HID 64 B 帧） |
| PHI2 | 外部晶振 1 MHz（→ 高 26 音分） | PIO 985 248 Hz（→ +0.04 音分） |
| 自动烧录 | 无 | HID `CMD_BOOTSEL` |
| 普通 PSID 时序 | ❌ 协议上限 | ✅ 可做到 |

SPFM 那套（`src/`, `docs/交接文档.md`）没有废弃，仍然是**已验证、可交付**的方案；
RP2350A 是它的继任者，解决 SPFM 解决不了的那一项。

## 下一步

1. 用 `sid_stream.exe` 把 `sid/` 下几十首跑一遍，听有没有掉音、跑调（当前首要）
2. 把 `write_reg_sid()` 的 20 µs 往下扫，找到最快可用值（提高 digi 密度上限）
3. 写一个正式的 RP2350 播放器：目录连播、按键、WS2812 状态
4. 接 `.sid` 直接播放（复用 `src/sid_rsid.c` 的 6502 提取）
