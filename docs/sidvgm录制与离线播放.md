# sidvgm 录制与离线播放

两个独立工具，都不需要硬件，都不影响正在运行的播放器：

| 工具 | 作用 |
|---|---|
| `build\sidextract.exe` | `.sid` → `.sidreg` / `.sidvgm`，以及 `.sidvgm` 的 dump 与回转 |
| `build\sidvgmplay.exe` | 用软件 SID 模型播放 `.sidvgm` / `.sidreg`，输出 WAV 或直接出声 |

格式规范见 [sidvgm文件规范.md](sidvgm文件规范.md)。

## 0. 默认引擎：libsidplayfp + reSIDfp

**参考标准是 `build\lsfp_to.wav`** —— 用原版 libsidplayfp 3.1.2 + reSIDfp 1.3.0
渲染的 Turbo Outrun（20 秒），和 DeepSID / sidplayfp 听到的一致。
所有"录得对不对、像不像"的判断以它为准。

现在两个工具默认就用这套：

| | 默认 | 说明 |
|---|---|---|
| 录制 `sidextract` | `--engine lsfp` | libsidplayfp 自己的 6502/VIC/CIA 跑歌，捕获 `$D400-$D41F` 写入 |
| 播放 `sidvgmplay` | `--model lsfp` | reSIDfp 出声，和标杆同一个核 |
| 播 `.sid` | 直通 libsidplayfp | 不走录制，直接出声，**和标杆 WAV 逐字节相同** |

```powershell
# 标杆渲染器（就是官方的 config/load/play/mix 调用，没有我们自己的 SID 代码）
.\build\lsfpref.exe sid\turboutrun.sid to_ref.wav 20

# 现在我们自己的默认路径，输出与上面逐字节相同
.\build\sidvgmplay.exe sid\turboutrun.sid --wav ours.wav --seconds 20
```

### 0.1 录制路径和直通路径为什么不是逐字节相同

直通是"边跑 6502 边出声"，一次成型；录制是"先把写入流存下来，再按 1/44100 秒的
时间戳重放"。两边的寄存器内容和顺序完全一致，但重放时一条写入的落点被量化到
采样 tick 上，再加上 t=0 取在 init 结束处，整条流会和直通差一帧以内。

实测 Turbo Outrun 20 秒：包络互相关系数 0.993，偏移 20 ms；RMS 1081 vs 1004；
瞬态 45 vs 55。**听得出来是同一首、鼓点齐全**，但不是逐字节相同。

**要听"标准答案"用直通（默认）；要发给真芯片用录制（`--record` / `sidextract`）。**

## 0.2 WebSid 不严肃，不要拿它当标准

实测同一首 `sid\turboutrun.sid`，20 秒。瞬态检测器固定为：10 ms 不重叠窗的
RMS，后一窗超过前一窗 3 倍且绝对值 > 600 记一次。

| 引擎 | digi 检测 | RMS | 峰值 | 鼓点瞬态 |
|---|---|---|---|---|
| **libsidplayfp + reSIDfp**（标杆 `build\lsfp_to.wav`） | — | 1004 | 14352 | **55** |
| WebSid（原版入口 `computeAudioSamples`，`build\ws_to.wav`） | `type=0 NONE` | 2043 | 28925 | **0** |

WebSid 把这首的数字鼓整个丢了：它自己的 digi 检测器报 `NONE`，`$D418` 全程只写
了一次；鼓声依赖的是 `$D416/$D417` 的快速滤波扫描，WebSid 的实现直接压掉了。
所以 WebSid **不能**作为录制或音频的参考，只能当"帧内时间戳更细"的实验品。

两个 WAV 连起来听一秒就能分出来。

## 0.3 三套引擎都保留

录制（6502 + SID 写入捕获）和音频（寄存器 → 采样）是两件事，各自可切：

| | 后端 | 说明 |
|---|---|---|
| 录制 | `lsfp`（**默认**） | libsidplayfp 的 6502/VIC/CIA，参考实现 |
| 录制 | `websid` | 上游 WebSid 的 CPU/VIC/CIA，帧内时间戳最细，但 digi 有坑 |
| 录制 | `native` | 本项目手写的 fake6502 核心（`src/sid_rsid.c`） |
| 音频 | `lsfp`（**默认**） | reSIDfp，标杆同款 |
| 音频 | `websid` | 上游 WebSid 的 SID 实现 |
| 音频 | `native` | 本项目手写的 `src/sidmodel.c` |

```powershell
# 默认就是 libsidplayfp + reSIDfp
.\build\sidextract.exe sid\Golden_Axe.sid ga.sidvgm --seconds 30
.\build\sidvgmplay.exe ga.sidvgm --wav ga.wav

# 回到 WebSid / 手写引擎
.\build\sidextract.exe sid\Golden_Axe.sid ga_ws.sidvgm --engine websid
.\build\sidvgmplay.exe ga_ws.sidvgm --model websid
.\build\sidextract.exe sid\Golden_Axe.sid ga_native.sidvgm --engine native
.\build\sidvgmplay.exe ga.sidvgm --model native
```

`--engine` 管录制，`--model` 管音频，可任意组合。
`sidvgmplay` 播放 `.sid` 时的自动转换也认 `--engine`。

上游源码原样放在 `third_party/upstream/`（含两个 release tarball 和 zip），
静态库在 `third_party/local/{lib,include}`。构建方法见
[libsidplayfp构建.md](libsidplayfp构建.md)。

缓存文件名带引擎和版本标记（`_lsfpv3` / `_websidv3` / `_nativev3`），所以升级
录制器后旧缓存不会再被读到；`--reconvert` 强制覆盖重录。


## 1. 录一首

```powershell
cd E:\working\zcode-projects\spfm_sid6581

# 60 秒（默认）
.\build\sidextract.exe sid\Golden_Axe.sid ga.sidvgm

# 指定长度和子曲
.\build\sidextract.exe sid\R-Type.sid rtype.sidvgm --seconds 120 --subsong 3

# 自定义 GD3 注释
.\build\sidextract.exe sid\Robots.sid robots.sidvgm --seconds 90 --comment "my recording"
```

输出容器由**文件扩展名**决定：`.sidvgm` 写二进制，其它扩展名写旧的文本流。
不输出文件名时只打印统计，用来确认解析正常。

`--seconds` 是渲染时长。`.sid` 本身没有长度字段，渲染多长就录多长；
之后要剪短或补长，用别的工具处理，录制器不猜。

## 2. 看一眼录了什么

```powershell
# 头部 + 前 20 条命令
.\build\sidextract.exe ga.sidvgm --dump --cmds 20

# 全部命令
.\build\sidextract.exe ga.sidvgm --dump
```

dump 会打印：版本、EOF/GD3/data/额外头偏移、地区标记、tick 率、总 tick 数、
循环字段、每颗芯片的类型与时钟、GD3 标签，然后是命令流。

想确认某首歌的路由没出错，看 `chips` 一行和头几条 `$18` 写入即可。

## 3. 转回文本

```powershell
.\build\sidextract.exe ga.sidvgm ga_back.sidreg
```

可以和直接渲染的 `.sidreg` 做 diff：寄存器序列应完全一致，时间差不超过
0.0114 ms（半个 tick）。

## 4. 离线播放（软件 SID 模型）

```powershell
# 直接出声，整首
.\build\sidvgmplay.exe ga.sidvgm

# 只放前 30 秒
.\build\sidvgmplay.exe ga.sidvgm --seconds 30

# 从 60 秒开始
.\build\sidvgmplay.exe ga.sidvgm --start 60

# 渲染成 WAV（不占用声卡，可反复听）
.\build\sidvgmplay.exe ga.sidvgm --wav ga.wav

# 只看头部
.\build\sidvgmplay.exe ga.sidvgm --info
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--wav FILE` | — | 渲染成 16-bit 单声道 44.1 kHz WAV；多输入时自动加 `_1`、`_2` 后缀 |
| `--seconds N` | 整条流 | 每首时长；超过流长时会继续渲染，让最后一个音自然衰减 |
| `--start N` | 0 | 起始秒数；会把起点之前的写入全部快进应用一遍 |
| `--subsong N` | 头部默认 | `.sid` 转换时的子曲序号，0 起 |
| `--model NAME` | `lsfp` | 音频后端：`lsfp`（reSIDfp）、`websid` 或 `native` |
| `--engine NAME` | `lsfp` | `.sid` 自动转换用的录制器：`lsfp`、`websid` 或 `native` |
| `--record` | 关 | `.sid` 先录成流再播放（真芯片走的就是这条）；默认是直通 libsidplayfp |
| `--no-cache` | 关 | 不读也不写 `sid_cache\` |
| `--8580` | 6581 | 换 8580 的滤波器曲线 |
| `--no-filter` | 关 | 旁路滤波器，听原始波形 |
| `--info` | — | 只打印头部，不播放 |

`.sidreg` 也能直接喂给它，时钟按 PAL / 985248 Hz 处理。

### 直接喂 `.sid`：默认直通，`--record` 才先录

```powershell
# 默认：.sid 交给 libsidplayfp 自己的播放器，一次成型
.\build\sidvgmplay.exe sid\Golden_Axe.sid
.\build\sidvgmplay.exe sid\Golden_Axe.sid --wav ga.wav --seconds 30

# 要和真芯片走同一条路（先录成寄存器流，再重放）
.\build\sidvgmplay.exe sid\Golden_Axe.sid --record
```

直通模式下输出与 `lsfpref.exe` 逐字节相同，所以听到的就是标杆。

`--record` 模式会先把 `.sid` 录成流，存到
`sid_cache\<名字>_s<子曲>_d<秒>_<引擎>v<版本>.sidvgm`，下次同一个文件直接读缓存：

```
[1/1] sid/Golden_Axe.sid
  cached: sid_cache/Golden_Axe_s0_d180_lsfpv3.sidvgm
  title  : Golden Axe
```

换长度就改 `--seconds`，换录制器就用 `--engine websid` / `--engine native`，
缓存文件名都会跟着变，互不覆盖；`--no-cache` 关掉读写缓存；`--reconvert`
无视已有缓存强制重录；子曲用 `--subsong N`（0 起）。

### 列表播放

给多个文件、或者给一个目录，就按顺序连播：

```powershell
# 两首
.\build\sidvgmplay.exe ga.sidvgm rtype.sidvgm

# 整个目录（.sid / .sidvgm / .sidreg 混在一起也行，按文件名排序）
.\build\sidvgmplay.exe sid

# 目录 + 每首 60 秒
.\build\sidvgmplay.exe sid --seconds 60

# 批量出 WAV，自动编号：pl_1.wav、pl_2.wav ...
.\build\sidvgmplay.exe sid --wav pl.wav --seconds 60
```

播放中：

| 键 | 作用 |
|---|---|
| `n` | 下一个文件（当前曲目立即切断） |
| `b` | 上一个文件 |
| `,` | 同文件的上一个子曲（仅 `.sid`，重新转换并播放） |
| `.` | 同文件的下一个子曲（仅 `.sid`） |
| `q` / `Esc` | 退出 |

这套键和硬件播放器 `sidlist` 完全一致：`n`/`b` 换文件，`,`/`.` 换子曲。
`.sidvgm` / `.sidreg` 是单条流，没有子曲，按 `,`/`.` 会提示
`has no subsongs` 并跳到下一个文件。

目录里混着 `.sid` 时，每首播放前先转换（仅首次），之后都走缓存。
`--info` 只打印每首头部，不播放。

> 这个模型是**听播放内容对不对**用的，不是芯片复刻：振荡器、ADSR、脉冲宽度、
> 噪声、环调、硬同步、状态变量滤波器都按周期建模。`--model lsfp`（默认）用的是
> reSIDfp，那是真芯片级别的；`--model native` 才是这个近似模型，只用来对照。

## 5. 实时播放的缓冲模型

`sidvgmplay` 不带 `--wav` 时走 waveOut，缓冲结构照 libvgm 的 DirectSound
后端：挂 4 个 1024 帧的缓冲，靠 `WAVEHDR.dwFlags`（对应 DirectSound 的填充
指针）判断哪个缓冲区已经播完、可以重新填。

时间推进按绝对 tick 算，不是"每个事件睡一下"：每个渲染块正好消耗
`block` 个 tick，所以某一块慢了只会让后面的音符挤在一起，不会整体漂移。

`--wav` 走的是同一套渲染，只是把块直接写进文件，速度大约是实时的 40 倍。

## 6. 已验证的范围

10 首样例（Golden Axe、R-Type、LED Storm、Yie Ar、atmos、Robo3、Robots、
Giana 两份、Turbo Out Run）在 20 秒长度上：

- `sid → sidvgm → sidreg` 与 `sid → sidreg` 事件数相同、寄存器与数据序列
  逐条相同；
- 最大时间偏差 0.0114 ms；
- 440 Hz 单音测试成立（测频 440.0 Hz），ADSR 慢释放（R=F）在 4 秒内单调
  衰减，门控关闭后正确进入 Release；
- 滤波器路由：`$D417` 对应声部打开后输出电平变化，`--no-filter` 回到未滤
  波电平。

libsidplayfp 录制路径（现在的默认）另有两条硬指标：

- **可重复**：同一首录两次，`.sidvgm` 逐字节相同（`powerOnDelay` 固定为
  `MAX_POWER_ON_DELAY`，否则 libsidplayfp 会用时间种子随机化上电延迟）；
- **直通即标杆**：`sidvgmplay <tune.sid> --wav` 与 `lsfpref.exe` 同名输出
  逐字节相同（Turb Outrun、Echoes、Giana、Golden Axe 都验过）。

未验证的部分：与真实 6581/8580 的音色 A/B。

## 7. 三引擎对照

libsidplayfp、WebSid 与 native 录同一批曲子的寄存器流，比较 `$D400-$D41F`
的寄存器与数据序列和每条的时间戳。native vs websid 的实测（10 秒）：

| 曲子 | 事件数（两边相同） | 寄存器/数据序列 | 时间差 最大 / 平均 | 时间戳唯一值 native → websid |
|---|---|---|---|---|
| Golden_Axe | 11575 | 逐条相同 | 2.67 / 1.56 ms | 3538 → 11554 |
| R-Type | 8079 | 逐条相同 | 1.88 / 1.02 ms | 2158 → 8052 |
| turboutrun | 8575 | 逐条相同 | 2.22 / 0.74 ms | — |
| atmos | 12059 | 逐条相同 | 0.83 / 0.45 ms | 1503 → 12025 |
| Robo3 | 11048 | 逐条相同 | 0.84 / 0.47 ms | — |
| Yie_A | 7850 | 逐条相同 | 3.13 / 0.66 ms | — |

结论：**两边写出来的寄存器和数据完全一致**，差别只在一帧之内每条写入的
时间戳精度。WebSid 和 libsidplayfp 都保留帧内真实间隔（atmos 12059 条里
12025 个不同时刻），native 只有 1503 个——native 把一帧内的多次写入挤到了
同一时刻。

**但时间戳细不等于声音对**（见 §0.2）：digi / 鼓曲目只有 libsidplayfp 对。
`sid/rsid/` 下的 RSID 文件 WebSid 和 libsidplayfp 都能跑（3 秒长度全部通过）。

当前默认是 **libsidplayfp（录制 + 音频）**；`websid` 和 `native` 保留为选项。

### 体积与耗时（60 秒 Golden Axe，同一首）

| 引擎 | 事件数 | 文件大小 | 录制耗时 |
|---|---|---|---|
| `lsfp` | 约 71000 | 322 KB | 1.9 s |
| `websid` | 71027 | 340 KB | 2.7 s |
| `native` | 71073 | 241 KB | 0.2 s |

`lsfp` 和 `websid` 都保留了帧内的真实间隔，所以**每条写入之间都要插一个 wait**；
`native` 把同一时刻的一组写入并在一起、整体只等一次，文件因此小 30%，
代价是帧内时间戳只有 1/8 的精度。录制速度上 `native` 最快，`lsfp` 次之，
`websid` 最慢（三者都是逐周期跑 CPU）。
