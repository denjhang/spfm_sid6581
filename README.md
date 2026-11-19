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
sidtest.exe sidplay sid/Commando.sid              :: 播 60 秒
sidtest.exe sidplay sid/R-Type.sid --seconds 120  :: 指定时长
sidtest.exe sidplay sid/R-Type.sid --subsong 3    :: 换子曲（0 起）
sidtest.exe sidplay sid/R-Type.sid --speed 1.5    :: 1.5 倍速
sidtest.exe sidplay sid/Robots.sid --loops 2      :: 循环两遍
sidtest.exe sidlist sid                           :: 整个目录连着放
sidtest.exe sidlist sid/rsid --from 10            :: 从第 11 首开始
```

播放列表里按键：`n` 下一首、`b` 上一首、`,` 上一个子曲、`.` 下一个子曲、`q` 退出。

| 参数 | 作用 | 默认 |
|---|---|---|
| `--seconds N` | 提取/播放多长 | 60 |
| `--subsong N` | 子曲序号，0 起 | 文件默认 |
| `--speed X` | 播放倍速 | 1.0 |
| `--loops N` | 循环次数 | 1 |
| `--no-cache` | 不读也不写 `.sidreg` 缓存 | 关 |
| `--cache-dir DIR` | 缓存目录 | `sid_cache` |

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
