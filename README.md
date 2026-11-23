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
| [docs/播放器使用说明.md](docs/播放器使用说明.md) | **PowerShell 用法**：`sidplay`/`sidlist`/`sidextract`、定时器与发送粒度、排错 |
| [docs/sidvgm文件规范.md](docs/sidvgm文件规范.md) | **格式规范**：VGM 式容器、相对偏移、0x65/66/67 芯片 ID、0x41/42/43 写命令、GD3 |
| [docs/sidvgm录制与离线播放.md](docs/sidvgm录制与离线播放.md) | **录制 + 离线播放**：默认 libsidplayfp + reSIDfp（与标杆逐字节一致）；`--engine` / `--model` 可切回 websid / native |
| [docs/libsidplayfp构建.md](docs/libsidplayfp构建.md) | **音质标杆**：原版 libsidplayfp 3.1.2 + reSIDfp 1.3.0 的构建方法，以及为什么 WebSid 不能当标准 |
| [docs/RSID判定与体积对比.md](docs/RSID判定与体积对比.md) | **123 首实测**：RSID 的真正标志是 `play=0000`（不是头 magic），以及新引擎相对旧引擎的体积膨胀（常规 1.45×，digi 曲 6–22×） |
| [third_party/upstream/README.md](third_party/upstream/README.md) | 上游 WebSid / libsidplayfp / libresidfp 原样源码 + 压缩包（换版本时从这里重取） |

## 当前状态

**✅ 已上机验证通过：三个声部同时演奏正常，`.sid` 播放已跑通；
带中断、带数字鼓的曲目（PSID 与 RSID）离线回放全部正常。**

- 写时序已解决，单次写入（`--repeat 1`）稳定，音高正确
- `music` 命令三声部同时工作：脉冲波旋律 + 三角波低音 + 噪声打击乐
- 曲目：小星星、欢乐颂、生日快乐、巴赫（移植自 `Arduino_SID/SID_Bach`）
- `sidplay` 可以直接放 C64 的 `.sid` 文件：内嵌 6502 跑一遍，把寄存器流按时序推到
  真芯片上；10 秒 R-Type 实测最大滞后 2 ms（一帧 19.96 ms）
- 离线引擎换成了 libsidplayfp 3.1.2 + reSIDfp 1.3.0：CIA 中断、RSID、数字鼓
  （`$D418` 采样和 `$D416/$D417` 滤波扫描两种）都能还原，
  输出与官方 `lsfpref.exe` 逐字节相同

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
| `--no-cache` | 不读也不写 `.sidreg` 缓存 | 关 |
| `--cache-dir DIR` | 缓存目录 | `sid_cache` |

写协议是唯一需要按硬件调的参数：这套接线下 SID 是异步写器件，`1step` 实测兼容性
更好（`2step` 会放坏一部分曲子）；换回旧的四芯片接线时用 `--proto 2step`。
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
make            # 硬件播放器 + 两个离线工具
make tools      # 只重新编两个离线工具（播放器正在跑时用这个）
make ref        # 只编参考渲染器 build/lsfpref.exe
```

产物在 `build/`：

| 文件 | 作用 |
|---|---|
| `sidtest.exe` | 硬件播放器（已静态链接，只需同目录的 `ftd2xx64.dll`） |
| `sidextract.exe` | `.sid` → `.sidreg` / `.sidvgm` |
| `sidvgmplay.exe` | 播放 `.sidvgm` / `.sidreg` / `.sid`，输出 WAV 或实时出声 |
| `lsfpref.exe` | 音质标杆渲染器（`make ref`），原版 API，不含本项目 SID 代码 |

两个离线工具的 SID 引擎用第三方的 libsidplayfp 3.1.2 + reSIDfp 1.3.0，
静态库和头文件已经放在 `third_party/local/`，**直接 `make` 就能编，不需要先建库**。
要从上游 release tarball 重新建库（换版本时）：

```bash
/d/msys64/usr/bin/bash.exe tools/build_libsidplayfp.sh
```

必须用 MSYS2 的 bash；Git Bash 的 autoconf 看不到 gcc。细节和三个坑见
[docs/libsidplayfp构建.md](docs/libsidplayfp构建.md)。

## 离线播放 / 试听（不需要硬件）

```powershell
# 实时出声，.sid 直通 libsidplayfp，和标杆逐字节相同
.\build\sidvgmplay.exe sid\turboutrun.sid

# 整目录连播，每首 60 秒
.\build\sidvgmplay.exe sid --seconds 60

# 出 WAV
.\build\sidvgmplay.exe sid\turboutrun.sid --wav to.wav --seconds 20

# 和真芯片走同一条路：先录寄存器流，再重放
.\build\sidvgmplay.exe sid\turboutrun.sid --record

# 8580 滤波曲线 / 只打印标题
.\build\sidvgmplay.exe sid\Golden_Axe.sid --8580
.\build\sidvgmplay.exe sid --info
```

按键：`n` 下一个文件、`b` 上一个、`,` / `.` 切子曲（仅 `.sid`）、`q` 退出。

`--engine lsfp|websid|native` 选录制器，`--model lsfp|websid|native` 选音频后端，
默认都是 `lsfp`。**带中断、带数字鼓的曲目只有 lsfp 是对的**（WebSid 在
`turboutrun.sid` 上鼓点瞬态 0 vs 55）。详见
[docs/sidvgm录制与离线播放.md](docs/sidvgm录制与离线播放.md)。

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
