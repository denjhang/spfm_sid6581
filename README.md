# SPFM SID 6581 驱动

SID 6581 / 8580 挂到 SPFM 并行总线的驱动 + 控制台冒烟测试。

**硬件：74LS374 + 74HCT02 + 74HCT32**（地址锁存 + 相位译码），
已上机验证。SID 接在 **slot 3**（RE-SPFM 最后一个槽），PHI2 = **1.000 MHz**。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/写入时序调试记录.md](docs/写入时序调试记录.md) | **实测记录**：为什么只有 repeat 能稳定发声、SPFM 与 SID 的时序不匹配分析 |
| [docs/接线表_374_02_min.md](docs/接线表_374_02_min.md) | **✅ 已验证的接线表（现行）**：374 + 02 + 32，三颗 IC |
| [docs/接线表_374_02_74.md](docs/接线表_374_02_74.md) | 方案索引：三颗 IC 的由来，以及两轮误判的记录 |
| [docs/接线表_374_02_123.md](docs/接线表_374_02_123.md) | 旧改进方案（74HC123 单稳态，**已废弃**） |
| [docs/接线表_374_02.md](docs/接线表_374_02.md) | 最早的接线（问题来源，仅作历史记录） |
| [docs/交接文档.md](docs/交接文档.md) | 项目状态、寄存器速查、风险清单 |
| [docs/驱动说明.md](docs/驱动说明.md) | 驱动 API、写模式、字节流验证方法 |

## 当前状态

**✅ 已上机验证通过：三个声部同时演奏正常，`.sid` 播放已跑通。**

- 写时序已解决，单次写入（`--repeat 1`）稳定，音高正确
- `music` 命令三声部同时工作：脉冲波旋律 + 三角波低音 + 噪声打击乐
- 曲目：小星星、欢乐颂、生日快乐、巴赫（移植自 `Arduino_SID/SID_Bach`）
- `sidplay` 可以直接放 C64 的 `.sid` 文件：内嵌 6502 跑一遍，把寄存器流按时序推到
  真芯片上；10 秒 R-Type 实测最大滞后 2 ms（一帧 19.96 ms）

问题曾是：只有 `--repeat` ≥ 4 才能稳定发声，且音高不跟随指令。根因是 `/CS` 与 `R/W`
两路不同源（一路过两个 NOR 门、一路裸接 `/WR`），且 `/CS` 在两相之间一直是低的。

现在的做法（详见 [接线表_374_02_min.md](docs/接线表_374_02_min.md)）：

1. `LE = NOR(A0, /WR)` 在地址相给 374 一个上升沿，锁住 `addr & 0x1F`；
2. `TRIG_n = ¬A0 ∨ /WR ∨ CSn` 把写条件编码成一个信号，**直连 SID 的 `/CS`（pin6）**；
3. SPFM 的 `/WR` **直连 SID 的 `R/W`（pin7）**；
4. `PHI2` 由 1 MHz 晶振独立供给（pin8）。

**三颗 IC：74LS374 + 74HCT02 + 74HCT32。** 不需要 74HC74，不需要移位寄存器，不需要单稳态。

> 早期版本用过 74HC74 整形，还把 `TRIG_n` 同时接到 `/CS` 和 `R/W`。2026-11-20 实测
> 拆掉 74HC74、把 `/WR` 单独接 `R/W` 后**无任何副作用**，同时写入更稳定。
> 这说明 6581 在这里就是异步写器件，`/CS` 与 `R/W` 同时为低即锁存，
> 不存在"必须等 PHI2 沿"的约束——之前那个结论是从 FPGA 软核 RTL 推出来的，过于保守。

## 快速上手

先切到 exe 所在目录（`ftd2xx64.dll` 必须和 exe 同目录，已经放好了）：

```cmd
cd /d E:\working\zcode-projects\spfm_sid6581\build
```

**① 确认连上硬件**

```cmd
sidtest.exe list
```

能看到 FTDI 设备就说明 SPFM 在线。看不到就是 USB / 驱动问题。

**② 列出所有曲子**

```cmd
sidtest.exe music --list
```

```
  twinkle   Twinkle Twinkle Little Star
  ode       Ode to Joy
  birthday  Happy Birthday
  bach      Bach (SID_Bach transcription)
```

**③ 播放**

```cmd
sidtest.exe music --song twinkle
sidtest.exe music --song ode
sidtest.exe music --song birthday
sidtest.exe music --song bach
```

不加 `--song` 默认播小星星。**切换曲目就是换 `--song` 后面的名字。**

| 参数 | 作用 | 默认 | 例子 |
|---|---|---|---|
| `--song NAME` | 选曲目 | `twinkle` | `--song bach` |
| `--bpm N` | 速度 20–400 | 120 | `--bpm 90` 慢 / `--bpm 180` 快 |
| `--loops N` | 重复次数 | 1 | `--loops 3` 连播三遍 |
| `--list` | 只列曲目不播放 | — | `music --list` |

组合例子：

```cmd
sidtest.exe music --song bach --bpm 90
sidtest.exe music --song twinkle --bpm 150 --loops 2
```

参数顺序随便，`--song bach music` 也能跑，但建议写成 `music --song bach`。

**④ 不接硬件也能验证**（只打印发出去的字节流）

```cmd
sidtest.exe --dry-run music --song bach
```

## 播放 C64 的 .sid 文件

`sid/` 下面有 6 首单曲和 `sid/rsid/` 里 93 首 RSID。`.sid` 里不是音符而是 6502 代码，
所以播放流程是：**内嵌 6502 跑一遍 → 记录每一条寄存器写入和时间戳 → 按原速推给真芯片**。
全程纯 C，不依赖 Python。

```cmd
sidtest.exe sidinfo sid/Commando.sid              :: 看标题、PAL/NTSC、子曲数
sidtest.exe sidplay sid/Commando.sid              :: 无限播，按键停
sidtest.exe sidplay sid/R-Type.sid --seconds 120  :: 只播 120 秒
sidtest.exe sidplay sid/R-Type.sid --subsong 3    :: 换子曲（0 起）
sidtest.exe sidplay sid/R-Type.sid --speed 1.5    :: 1.5 倍速
sidtest.exe sidlist sid                           :: 整个目录连着放
sidtest.exe sidlist sid/rsid --from 10            :: 从第 11 首开始
```

播放列表里按键：`n` 下一首、`b` 上一首、`,` 上一个子曲、`.` 下一个子曲、`q` 退出。
不带 `--seconds` 时每首一直播到你按键，没有时长上限，也不去猜循环点。

| 参数 | 作用 | 默认 |
|---|---|---|
| `--seconds N` | 提取/播放多长 | 0 = 无限 |
| `--subsong N` | 子曲序号，0 起 | 文件默认 |
| `--speed X` | 播放倍速 | 1.0 |
| `--proto P` | 写入协议：`1step`（一条 Path A）或 `2step`（两条 Path B） | `1step` |
| `--rsid` | 改用 RSID 模型（完整时钟 + 中断，digi 采样能跑） | 关 |
| `--limit HZ` | 把 `$D418` 更新率钉在 HZ 以内（配合 `--rsid` 用） | 关 |
| `--no-cache` | 不读也不写 `.sidreg` 缓存 | 关 |
| `--cache-dir DIR` | 缓存目录 | `sid_cache` |
| `--length auto` | 播到 HVSC 数据库给的长度自动停 | 关 |
| `--db FILE` | HVSC 的 `Songlengths.md5` | 无 |
| `--stil FILE` | HVSC 的 `STIL.txt`（子曲名/作者） | 无 |
| `--slot2 N` `--slot3 N` | 把多 SID 曲的第 2/3 颗放到 SPFM 槽 N | 关（不播）

**标题、多长、子曲叫什么**，PSID 头里都没有（头里也没有循环点）。`sidmeta.c`
补了两个外部来源，做法和参考播放器一致：

- **长度**：`SONGLENGTHS/Name.ssl`（同目录，2 字节 BCD/子曲）优先，退到
  `Songlengths.md5`（`<整个文件的MD5>=mm:ss ...`，HVSC #71 起的唯一格式）。
- **子曲名**：`STIL.txt` 的 `TITLE:`/`ARTIST:`，按 `(#N)` 分块。

没有数据库时不做任何推测：`--seconds` 给了就播那么多，没给就一直播到你按键——
libsidplayfp 的默认定时器同样写死 `0 = FOREVER`。**我们不做循环点检测**：SID
头没有这个字段，WebSid 和 libsidplayfp 也都不做，HVSC 的“长度”本身就是人工
试听把循环点折成的一个秒数。

```cmd
build\sidextract.exe sid/Robots.sid --info --db Songlengths.md5 --stil STIL.txt
::   length  : 0:52.403  (subsong 1)
::   STIL    : The Robots [from The Man-Machine] / Kraftwerk

sidtest.exe sidplay sid/Golden_Axe.sid --length auto --db Songlengths.md5
:: 播到数据库给的 4:15 自动停

sidtest.exe sidplay sid/rsid/Anthrox_2SID.sid --slot2 1
:: 2SID 曲子：第一颗在槽 3，第二颗放槽 1
```

实测：`Golden_Axe` 在 Songlengths 里是 4:15（子曲 1）/ 0:27（子曲 2），`R-Type`
是 2:11 / 0:36 / 0:26；`--length auto` 播到这些秒数自动停。

写协议是唯一需要按硬件调的参数：这套接线下 SID 是异步写器件，`1step` 实测兼容性
更好（`2step` 会放坏一部分曲子）；换回旧的四芯片接线时用 `--proto 2step`。

**`--rsid` 是另一套仿真模型，默认不开。** 默认（PSID）每视频帧调一次 play 就结束，
20 ms 里约 50 条寄存器写入。`--rsid` 会让机器在整个 tick 里一直跑并响应 NMI/IRQ，
这是 Giana Sisters 那种"音量寄存器 digi"鼓声唯一能出来的方式，写入量升到
**6800 条/秒**。

**链路容量（实测口径，别再用 187.5 kB/s）**：1.5 Mbaud 配 8N1 是 10 bit/字节，
线速 150000 B/s = 6.667 µs/字节，一条 Path A（4 字节）= 26.667 µs，所以
**上限 37500 条/秒**。而每次 flush 之后 FTDI 的 2 ms latency timer 会让线路空转，
所以真正决定吞吐的是**一批装多少**：256 条/批 ≈ 29000 条/s，4096 字节/批 ≈
34900 条/s。换成 kB/s 的硬上限是 **数据 37.5 kB/s / 线上 150 kB/s**（一条 ADPCM
命令只带一个数据字节，线上字节是它的 4 倍）；V0 形状实测 29.0 / 116 kB/s，V1 是
37.3 / 149 kB/s，差额全在每批白付的 2 ms。`sidplay` 每视频帧 flush 一次，Giana
最密的一帧 172 条 = 688 字节，线上 4.59 ms + 2 ms = 6.59 ms，只占 19.96 ms 帧预算
的 33%——**不限速其实也装得下**。

基线来自 `Denjhang_Music_Player_v16` 的 YM2608 ADPCM 预载（零节流、上机验证过，
V0 是 256 条/批、V1 是 4096 字节切片）。`sidtest bench` 在本机复现这两种形状：

```cmd
sidtest.exe bench              :: V1/V0/每帧批量 全部实测，附实测/理论比
sidtest.exe bench --quick      :: 每种两万条，快速看数
```

**这个协议传的是几十 kB/s，不是几百 kB/s**：150 kB/s 是线上字节，而 4 字节命令里
只有 1 字节是数据，所以**有效载荷上限 37.5 kB/s**；扣掉每批 2 ms latency 后，
实际是 29–34 kB/s。换成"每采样点写一次寄存器"的流式音频，采样率上限就是
**29–37.5 kHz**——任何 44.1 kHz 的 PCM/SSG digi 在这套链路上必然要丢点，
这点和 MDPlayer 里 `dacControl` 强制 44100 Hz 时基、SSG 被往返周期卡住是同一个
机制的三种表现。详细推导见 `docs/MDPlayer硬体写入链路考察.md` §6.1、§7.1。

**RSID digi 这两首不需要为带宽降速**。60 秒实测：Giana 6835 次/s（最忙一秒 7434，
占上限 20%；最忙一帧 176 条，含 2 ms latency 占帧预算 34%），Turbo Outrun
6350 次/s（最忙一秒 8182 = 22%；最忙一帧 192 条 = 36%），都还有 3 倍以上余量。
真正该做的是删冗余：Giana 有 **66%** 的 `$D418` 写是重复值，`--limit 8000`
能删掉 64% 的命令数而保留 **100%** 的 DAC 值变化；再往下压才开始真的丢信息
（4000 Hz 剩 69%，2000 Hz 剩 40%）。见 §7.2–§7.3。

**`--limit HZ` 是给 `--rsid` 配的限速**：只把 `$D418`（音量）的更新率钉在固定时基上，
同一格里后来的值就地覆盖、和当前保持值相同的直接丢。其它 24 个寄存器一条不动，
所以旋律不受影响。链路侧同时加了发送队列排空检查（对应 MDPlayer 的
`SendData() + IsBufferEmpty()`）。Giana 20 秒的实测：不限速 108616 条 `$D418`，
`--limit 2000` 剩 13236 条、峰值 2000/s，`--limit 200` 剩 1705 条、峰值 210/s。
细节见 `docs/驱动说明.md` §6.10。

```cmd
sidtest.exe sidplay sid/Great_Giana_Sisters-rsid.sid              :: 普通播放，鼓声不出来
sidtest.exe sidplay sid/Great_Giana_Sisters-rsid.sid --rsid       :: 鼓声出来
sidtest.exe sidplay sid/Great_Giana_Sisters-rsid.sid --rsid --limit 2000   :: 鼓声 + 限速
build\sidextract.exe sid/Great_Giana_Sisters-rsid.sid out.sidreg --rsid --limit 2000
```
启动时会打印实际用的协议。

离线渲染（不需要硬件，也不需要 FTDI）：

```cmd
build\sidextract.exe sid/R-Type.sid rtype.sidreg --seconds 120 --subsong 0
```

`.sidreg` 是纯文本 `时间(ms) 寄存器 值`，可以直接喂回 `sidplay`，也可以拿来差分对比：

```cmd
sidtest.exe sidplay rtype.sidreg
```

**性能**：30 秒的音乐提取约 30 ms（旧 Python/py65 版要几分钟）；实机 10 秒 R-Type
最大滞后 2 ms。缓存目录 `sid_cache/` 已在 `.gitignore` 里。

## 三声部分配

| 声部 | 波形 | 用途 | 包络 |
|---|---|---|---|
| 1 | 脉冲波 `0x40` | 旋律 | AD=0x08（A=0，D=300 ms）SR=0x06（S=0，R=204 ms） |
| 2 | 三角波 `0x10` | 低音 | AD=0x0A（D=1500 ms）SR=0x06 |
| 3 | 噪声 `0x80` | 打击乐 | 底鼓 0x0400/300 ms、军鼓 0x0E00/240 ms、踩镲 0x2400/168 ms |

**钢琴感来自 `sustain = 0`。** SID 的 sustain 是"按住键时保持的音量"：
填 15 会让音符一直满音量直到下一个音（风琴感），填 0 才会自然衰减到消失。
音符时值结束前一个 tick 还会主动降 gate，保证每个音都有干净的 0→1 重触发边沿。

## 编译

MinGW-w64（本机在 `D:\msys64`）：

```bash
export PATH="/d/msys64/mingw64/bin:/d/msys64/usr/bin:$PATH"
make
```

产物：`build/sidtest.exe`（已静态链接，只需同目录的 `ftd2xx64.dll`）。

## 用法

在 `build/` 目录下（Windows cmd）：

```cmd
sidtest.exe list                                # 枚举 FTDI 设备
sidtest.exe init                                # 复位 + 初始化 slot 3
sidtest.exe selftest                            # 不上硬件，验证生成的字节流
sidtest.exe music                               # 三声部小曲（默认小星星）
sidtest.exe music --song ode                    # 欢乐颂
sidtest.exe music --song birthday               # 生日快乐
sidtest.exe music --song bach                   # 巴赫（移植自 Arduino_SID/SID_Bach）
sidtest.exe music --list                        # 列出所有曲子
sidtest.exe music --bpm 160 --loops 3           # 改速度和重复次数
sidtest.exe scale                               # C 大调音阶（方波）
sidtest.exe scale --wave saw --filter           # 锯齿 + 低通滤波
sidtest.exe chord                               # C / Am / F / G / C 三声部和弦
sidtest.exe noise                               # 噪声频率扫描
sidtest.exe pwm                                 # 占空比扫描
sidtest.exe filter                              # 滤波器截止频率扫描
sidtest.exe adsr                                # 四组 ADSR 对比
sidtest.exe checkup                             # 上电五项检查（需要万用表/示波器）
sidtest.exe interactive                         # 手敲寄存器
sidtest.exe --dry-run music --song bach         # 不连硬件，打印解码后的总线动作
```

在 git bash 里把 `sidtest.exe` 换成 `./build/sidtest.exe` 即可。

常用选项：`--slot N`（默认 3）、`--device N`（默认 0）、`--write-mode`、`--gap N`、`--verbose`、`--repeat N`。

## 关键约束

- 复位后 `0x18`（音量）= 0，**不写就没有声音**
- 初始化要先清零全部寄存器（374 上电后 Q 输出不定）
- `TRIG_n = ¬A0 ∨ /WR ∨ CSn` 必须经 74HCT32 译码；直连 CSn 会在地址相写入垃圾值导致起音咔哒
- `TRIG_n` 只驱动 SID 的 `/CS`（pin6），`R/W`（pin7）接 SPFM 的 `/WR`。
  **不要再把 `TRIG_n` 同时接到两个脚**——一个 74HCT32 门带两个负载会拖慢边沿，
  那正是早期"单次写入失真"的真正原因
- SID 的 pin8 是 PHI2，不是写信号；接错则完全无时钟
- 6581 需要 +12V（8580 是 +9V），缺了完全无声且很难察觉
- `0x18` 的 bit4–7 是滤波器模式 + 声部 3 静音，**不是波形开关**
