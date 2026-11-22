# sidvgm 录制与离线播放

两个独立工具，都不需要硬件，都不影响正在运行的播放器：

| 工具 | 作用 |
|---|---|
| `build\sidextract.exe` | `.sid` → `.sidreg` / `.sidvgm`，以及 `.sidvgm` 的 dump 与回转 |
| `build\sidvgmplay.exe` | 用软件 SID 模型播放 `.sidvgm` / `.sidreg`，输出 WAV 或直接出声 |

格式规范见 [sidvgm文件规范.md](sidvgm文件规范.md)。

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
| `--wav FILE` | — | 渲染成 16-bit 单声道 44.1 kHz WAV |
| `--seconds N` | 整条流 | 播放时长；超过流长时会继续渲染，让最后一个音自然衰减 |
| `--start N` | 0 | 起始秒数；会把起点之前的写入全部快进应用一遍 |
| `--8580` | 6581 | 换 8580 的滤波器曲线 |
| `--no-filter` | 关 | 旁路滤波器，听原始波形 |
| `--info` | — | 只打印头部 |

`.sidreg` 也能直接喂给它，时钟按 PAL / 985248 Hz 处理。

> 这个模型是**听播放内容对不对**用的，不是芯片复刻：振荡器、ADSR（用 WebSid
> 的 LFSR 阈值 + 指数延迟表）、脉冲宽度、噪声、环调、硬同步、状态变量滤波器
> 都是按周期建模的；6581 滤波器的非线性传输曲线和组合波形的芯片级差异没有
> 复现。判断"音符、包络、时序是否正确"够用，判断"音色像不像真芯片"不够。

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

未验证的部分：与真实 6581/8580 的音色 A/B，以及 digi（`$D418` 采样）曲目
的听感。
