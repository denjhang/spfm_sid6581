# SPFM SIDVGM Player 使用说明

`build\spfmplayer.exe` 是**听音乐的程序**。`sidtest.exe` 是硬件调试和寄存器
试探工具（`scale`、`chord`、`checkup`、`pins` 那些），两者共用同一套驱动，
但定位不同：`spfmplayer` 只做一件事——把一个 SID 曲目放到真 6581/8580 上。

## 1. 三种输入，一个程序

| 输入 | 含义 | 播放方式 |
|---|---|---|
| `.sid` | 6502 代码 + 头 | 边跑边推：第 N+1 帧在仿真时，第 N 帧还在芯片上响 |
| `.sidvgm` | 本项目录好的寄存器流（VGM 形状容器） | 按流里的时间戳回放，可 `--loop` |
| `.sidreg` | 老的纯文本流 `时间(ms) 寄存器 值` | 同上 |
| 目录 | 目录里的 `*.sid` 按文件名排序 | 播放列表，按键切歌 |

```powershell
cd E:\working\zcode-projects\spfm_sid6581

.\build\spfmplayer.exe sid\Golden_Axe.sid              # 一直播，按键停
.\build\spfmplayer.exe sid\R-Type.sid --seconds 60      # 只播 60 秒
.\build\spfmplayer.exe sid\rsid                         # 整个目录连播
.\build\spfmplayer.exe sid --list                       # 先看目录里有什么
```

先确认设备：`.\build\spfmplayer.exe sid\Golden_Axe.sid` 启动时会打印
`device 0: ...`；不接硬件就用 `--dry-run`，它照常跑时间线，只是不发 USB。

## 2. 录什么由 `--engine` 决定

`.sid` 不是音符，是机器码，所以播放前必须先"跑一遍"得到寄存器写入流。
两个录制器：

| 值 | 引擎 | 行为 |
|---|---|---|
| `lsfp`（默认） | libsidplayfp 3.1.2 | 它自己的 6502 + VIC + CIA。`play()` 由曲目真正的 CIA/VIC 中断触发，`$D418` 的帧内写入一条不丢 |
| `native` | 内置 fake6502（`sid_rsid.c`） | 每帧只调一次 `play()`，帧内写入全丢；`play=0000` 的 RSID 靠猜入口 |

**默认是 `lsfp`，因为它覆盖了 `native` 做不到的两类曲子：**

- **digi 鼓**：鼓点是往 `$D418` 连续写样本值，一帧内几十次。内置核心每帧只
  跑一次 `play()`，那些写入根本不存在，所以鼓声是空的。同一首 Turbo Outrun，
  `lsfp` 记到 98035 次 `$D418` 写入，`native` 是 0。
- **真 RSID**：`play` 地址是 `0000`，曲目自己装 CIA/VIC 中断向量。没有
  "call play(0000)" 可以调，只有完整的中断模型才能按原来的速率跑。

**但普通 PSID 用 `native` 听着更好。** 这是人工试听的结论（2026-11-22），
不是波形比对得出的。普通 PSID 每帧只调一次 `play()`、没有帧内 `$D418`
写入，两套核心产出的寄存器流本来就接近，差别在声音细节而不是丢事件。
所以按曲子类型选：

| 曲子类型 | 用哪个 |
|---|---|
| 普通 PSID（无 digi 鼓、`play != 0000`） | `--engine native`，或旧的 `sidtest.exe sidplay` |
| digi 鼓 / 真 RSID（`play = 0000`） | `--engine lsfp` |

不确定就先 `sidextract --info` 看 `play` 地址，或者两套各听一遍。

```powershell
.\build\spfmplayer.exe sid\Golden_Axe.sid --engine native  # 普通 PSID，试听更好
.\build\spfmplayer.exe sid\turboutrun.sid                  # digi 鼓，必须 lsfp
```

`native` 还有第二个价值：它不依赖 libsidplayfp，整个引擎是纯 C。出问题时
可以拿它把"引擎"和"硬件/驱动"两个变量分开——旧版 `sidtest.exe sidplay`
走的就是这条路。

### 2.1 为什么边跑边推不会卡

`lsfp` 的录制是流式的：`sid_lsfp_stream_next()` 只仿真到下一帧边界，把这
一帧的写入交出来。一帧 6502 大约几十微秒，而一帧音乐有 19.96 ms（PAL），
仿真完全藏得进播放时间里。

时间戳是**从第一帧 play 突发算起**的绝对 PHI1 周期数，帧边界固定在
`origin + k * frame_cycles`，所以无论分块怎么取都不会漂。

> 已验证：`lsfp` 流式录制和前段 `sid_lsfp_extract()` 的整段录制逐条一致
> （R-Type / Turbo Outrun / Giana RSID / Ambient / Alcorythm / Golden Axe，
> 每首 10 s，0 mismatch，含时间戳）。

### 2.2 用 `--seconds` 会走另一条路

给了 `--seconds N` 就先整段录完再回放（`sid_lsfp_extract()`）。好处是能一次
拿到固定产物做对比，坏处是启动要等录完。不给就是无限流式播放，那才是日常
用法。

## 3. 播放列表

```powershell
.\build\spfmplayer.exe sid                           # sid\ 下所有 .sid
.\build\spfmplayer.exe sid\rsid --from 10            # 从第 11 首开始
.\build\spfmplayer.exe sid\rsid --seconds 20         # 每首 20 秒自动切
```

| 键 | 作用 |
|---|---|
| `n` | 下一首 |
| `b` | 上一首 |
| `,` | 上一个子曲 |
| `.` | 下一个子曲 |
| `q` / `Esc` | 退出 |

按键读取是独立的阻塞线程，所以帧中间按也立刻生效，不用等下一个音符。

**单曲也能换子曲**：`--subsong N` 只是起点，播起来之后 `,` / `.` 照样在同一个
文件里前后走。走到第一个 / 最后一个会打印 `first subsong` / `last subsong` 并
停在原地，不会越界退出。

```powershell
:: 从子曲 10 开始，然后按 '.' / ',' 在 11 个子曲之间走
.\build\spfmplayer.exe sid\games\Greenrunner.sid --subsong 10
```

`.sidvgm` / `.sidreg` 是录好的单条流，没有子曲概念，`n` / `b` 之外的键不起作用。

## 4. 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--seconds N` | 0（无限） | 播 N 秒；给了就变成先整段录制再回放 |
| `--subsong N` | 文件默认 | 子曲序号，0 起 |
| `--speed X` | 1.0 | 倍速 |
| `--slot N` | 3 | SPFM 槽位 0..3 |
| `--device N` | 0 | FTDI 设备序号 |
| `--engine lsfp\|native` | `lsfp` | `.sid` 的录制器 |
| `--proto 1step\|2step` | `1step` | 写入协议，见第 5 节 |
| `--flush command\|register` | `command` | 一帧一次 USB，还是每写一次 |
| `--loop N` | 0（一次） | 缓冲流重复次数，`-1` 无限 |
| `--raw-filter` | 关 | 丢掉曲目的滤波器设置，保留主音量 |
| `--timer hprec\|hybrid\|mmtimer\|vgmplay` | `hprec` | 等待原语 |
| `--repeat N` / `--gap N` / `--gap-rand N` | 1 / 0 / 0 | 旧的写入可靠性旋钮 |
| `--dry-run` | 关 | 不碰硬件，只走时间线 |
| `--list` | — | 只列目录里的 `.sid` |
| `--help` | — | 完整帮助 |

### 4.1 digi 鼓的时间精度（`--pad` 系列）

和 `sidtest.exe` 是**同一份引擎**（`src/spfm_pad.c`），参数、默认值、行为完全
一致，所以两边的调法可以互相照抄。三种输入都能用，`.sid` 不需要 `--seconds`：
libsidplayfp 是"渲染好一帧再播这一帧"，帧内每处间隔在播放前就知道了，填充照常
生效。唯一够不到的是帧边界（下一帧还不存在），而那是 ~20 ms 的静音，远大于
`--pad`，本来也不会被填。

```powershell
# 录好的流
.\build\spfmplayer.exe "sidvgm\rsid\turboutrun\turboutrun_s01.sidvgm" `
    --pad 1 --pad-window 50 --pad-local 50 --pad-scope digi

# 活体 .sid，一直播到按键
.\build\spfmplayer.exe sid\rsid\turboutrun.sid `
    --pad 1 --pad-window 50 --pad-local 50 --pad-scope digi

# 整个目录连播（每首一直播到按键，或给 --seconds 自动切）
.\build\spfmplayer.exe sid\games `
    --pad 1 --pad-window 50 --pad-local 50 --pad-scope digi
```

**唯一的限制在录制器**：`--engine native`（内置 6502）只看得到每帧一次的
`play()`，帧内的 `$D418` 写入全部丢失，那种情况下密度闸门永远打不开、`--pad`
等于没加。digi 曲目保持默认的 `--engine lsfp` 就行。

| 参数 | 默认 | 说明 |
|---|---|---|
| `--pad MS` | 关 | 填充 ≤MS 的间隔；**越大鼓越好，也越吃带宽**。隐含 `--dedup-vol` |
| `--pad-scope all\|digi` | `all` | `digi` 只填与 `$D418` 相邻的间隔 |
| `--pad-window MS` | 0（关） | 局部滑动窗口；配 `--pad-local` 治"速度不均匀" |
| `--pad-local PCT` | 100 | 窗口内允许的线速上限 |
| `--pad-duty PCT` | 75 | 全局平均上限（漏桶） |
| `--pad-lead MS` | 25 | 主机领先音乐多少 = 延迟 |
| `--pad-chunk KB` | 2 | 每次 USB 传输的块大小 |
| `--pad-burst MS` | 200 | 漏桶能攒多少 |
| `--pad-min MS` | 0 | 短于此的间隔不填 |
| `--pad-lag MS` | 0（关） | 线上时间领先音乐过多时暂停填充 |
| `--pad-partial` | 关 | 允许半填（默认全填或全不填） |
| `--pad-rate HZ` | 200 | `$D418` 密度达到多少才算鼓段落 |
| `--pad-hold MS` | 60 | 鼓停下后还继续填多久 |
| `--pad-always` | 关 | 忽略密度闸门，整首无条件填 |
| `--digi HZ` | 0（关） | 只把 `$D418` 按 1/HZ 重采样；**只会降低音质** |

**密度闸门默认开着**，这是它存在的意义：填充把"主机按截止时间发"换成"缓冲填满
就发"，只有 `$D418` 写得够密时才成立。PSID 曲目、或者 digi 曲目的前奏（Giana
RSID 开头约 4 秒）每秒只写几十次，照填会让芯片饿着等再成团吐出，听感是卡顿加
节奏不对。所以只有密度过 `--pad-rate` 的段落才切换成填充节拍，其余部分走原来
的主机截止时间，与不加参数完全一样。播完会打印实际生效的区间：

```
  pad: 77230 ms filler, 392126 gaps refused, gated on 4.0-159.0 s
```

整首都不够密时会打印 `(no dense $D418 passage: nothing padded)`，那就是按普通
路径播完了，参数等于没加——这是正确行为，不是故障。

#### 到这里就是尽头

`--pad` 之所以有效，是因为 **1 字节 = 6.667 µs** 这个 UART 时间基，鼓点间隔
（约 136 µs）比它粗一个数量级，量化误差约 5%，听不出来。

普通 PSID 的帧内节奏是**微秒级**的，和 6.667 µs 同量级。用比信号还粗的格子去量化，
一部分写被提前、一部分被推后，相序（哪些写挨在一起、间隔多久）保不住。
所以那种"整首曲子听着都在抖"的问题 **不是参数没调对，是协议上限**：
SPFM 是给雅马哈 YM 系列设计的写入数据流，主机只能发"写哪个寄存器写什么值"和 NOP，
`/WR`、`/CS`、`A0` 全由固件产生，**没有引脚控制，也没有 PHI2 概念**，
更没有回读通道。

在这个硬件上再调参数，只会让普通曲子更糟。完整论证和"换控制器"的路线见
[SPFM时序精度上限.md](SPFM时序精度上限.md)。

## 5. 写入协议：默认 `1step`

| 值 | 线上字节 | 什么时候用 |
|---|---|---|
| `1step` | `[slot,0x00,addr,data]` 一条 Path A，固件自己出两个总线周期 | 这套三芯片接线（374 + 02 + 32）实测最稳 |
| `2step` | `[slot,0x80,addr]` + `[slot,0x81,data]`，中间由主机控制 | 旧的四芯片接线 |

`2step` 会把地址相和数据相拆到两次 USB 传输，中间给主机留了空档。当前接线
下 SID 是异步写器件（`/CS` 和 `R/W` 同时为低就锁存），这个空档没有意义，
反而让一部分曲子出错。**换回旧接线才用 `--proto 2step`。**

## 6. 不接硬件验证

```powershell
.\build\spfmplayer.exe sid\Golden_Axe.sid --dry-run --seconds 5
```

`--dry-run` 下时间线照跑（所以能看出每帧的 `lag`），但字节只进内存。它适合
确认"这首歌能不能正常跑完、写入数量对不对"，硬件本身不用上。

## 7. 出问题怎么定位

| 现象 | 先看什么 |
|---|---|
| `no FTDI device found` | `build\ftd2xx64.dll` 与 exe 同目录；`sidtest.exe list` 能否枚举 |
| 有旋律没鼓（digi 曲目） | 是不是加了 `--engine native`，去掉即为 `lsfp` |
| RSID 曲目速度不对 / 不响 | 同上，`native` 不认识 `play=0000` |
| 音高全错 | 换 `--proto 2step` 试，那是 374 地址锁存相位问题 |
| 节奏对但音少 | `--flush register` 对照，判断批量发送是否被硬件吃掉 |
| `lag` 涨到几十 ms | 定时器被抢；保持默认 `--timer hprec`，关掉别的高负载程序 |
| 播一会儿就停 | 看是不是给了 `--seconds` |
| 2SID/3SID 曲目报错 | 播放器只驱动一个槽，这类曲目会被显式拒绝 |

## 8. 与 `sidtest.exe` 的关系

| | `sidtest.exe` | `spfmplayer.exe` |
|---|---|---|
| 定位 | 上机调试、寄存器试探、自检 | 听音乐 |
| 依赖 | 只有驱动 + FTDI | 驱动 + libsidplayfp + sidvgm |
| `.sid` 录制器 | fake6502（`sidplay`） | 默认 libsidplayfp，可切回 fake6502 |
| `.sidvgm` | 认 | 认，可 `--loop` |
| digi 鼓调参 | `--pad` 全套 | 同一份引擎（`spfm_pad.c`），参数完全一致 |
| 体积 | 258 KB | 3.5 MB（静态链接） |

调试一条通道、量一个引脚、扫一个时序，用 `sidtest`；只想听，用
`spfmplayer`。
