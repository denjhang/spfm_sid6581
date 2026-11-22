# sidvgm 文件规范 v1.00

`.sidreg` 的升级格式。目标不是"能被 libvgm 播放"——libvgm 里根本没有 SID 设备
（`emu/SoundDevs.h` 的 `DEVID_*` 从 0x00 排到 0x29，没有 SID）——而是**把 VGM
的容器结构完整搬过来**：二进制头 + 相对偏移 + 等待命令 + 额外头 + GD3 标签 +
循环点，换掉 `.sidreg` 那种"一行一条、没有元数据、没法表达循环"的文本流。

参考实现读的是 libvgm 的播放器，不是 VGM 官方文档：

| 行为 | 出处 |
|---|---|
| 相对偏移（偏移相对自己所在字段，而非文件头） | `player/vgmplayer.cpp:126-131` `ReadRelOfs` |
| 头字段位置 0x04/0x14/0x1C/0x34/0xBC | `ParseHeader()` `vgmplayer.cpp:280-345` |
| 额外头：长度 + 时钟表 + 音量表 | `ParseXHdr_Data32/16()` `vgmplayer.cpp:377-424` |
| 时钟表项 = 1 字节类型 + 4 字节时钟 | 同上，步长 0x05 |
| GD3 = "Gd3 " + 版本 + EOT + 11 个 UTF-16LE 串 | `LoadTags()` `vgmplayer.cpp:437-476` |
| 0x61 等待 N 样本（3 字节），0x66 结束并跳循环 | `_CMD_INFO[]` `vgmplayer_cmdhandler.cpp:120` |
| 0x67 数据块 = `67 66 type len32 data` | `Cmd_DataBlock()` |
| 所有数值小端 | `ReadLE16/32` |

## 1. 文件总布局

```
+0x000  主头（0xC0 字节，实际用到 0xBB）
+0x0C0  额外头（extra header）
+...    芯片时钟表
+...    芯片音量表（本格式保留，写空表）
+data   命令流          ← 主头 0x34 处指向这里
+gd3    GD3 标签        ← 主头 0x14 处指向这里
+EOF    文件结束        ← 主头 0x04 处指向这里
```

偏移全部是**相对偏移**：存进文件的值 = 绝对位置 − 该字段自己的偏移。
例如数据从 0x0D8 开始，0x34 处写 `0x0D8 - 0x34 = 0x0A4`；值为 0 表示"不存在"。
这是 VGM 的原样规矩，不要改成绝对偏移。

## 2. 主头

所有多字节整数都是 **小端**。未列出的字节一律写 0。

| 偏移 | 类型 | 字段 | 本格式的含义 |
|---|---|---|---|
| +0x00 | 4B | magic | ASCII `"Svgm"`（`53 76 67 6D`）。注意 VGM 是 `"Vgm "`，我们故意区分。 |
| +0x04 | u32 | EOF 偏移 | 相对 0x04；= 文件长度 − 4 |
| +0x08 | u32 | 版本 | `0x00000100`（1.00）。高 16 位主版本，低 16 位次版本 |
| +0x0C | u32 | SN76489 时钟 | **0**。本格式不使用，留给结构对齐 |
| +0x10 | u32 | YM2413 时钟 | **0** |
| +0x14 | u32 | GD3 偏移 | 相对 0x14；0 = 无标签 |
| +0x18 | u32 | 总 tick 数 | 命令流里所有等待的累计 |
| +0x1C | u32 | 循环偏移 | 相对 0x1C；0 = 不循环 |
| +0x20 | u32 | 循环段的 tick 数 | 从循环点起到结束的 tick 数 |
| +0x24 | u32 | **地区标记** | `50` = PAL，`60` = NTSC，`0` = 不校正。**不是采样率** |
| +0x28 | u16 | SN76489 反馈 | 0 |
| +0x2A | u8 | SN76489 位宽 | 0 |
| +0x2B | u8 | SN76489 标志 | 0 |
| +0x2C | u32 | YM2612 时钟 | 0 |
| +0x30 | u32 | YM2151 时钟 | 0 |
| +0x34 | u32 | 数据偏移 | 相对 0x34 |
| +0x38–0x7B | — | 其余芯片时钟/标志 | 全 0（SID 不进固定头，走额外头） |
| +0x7C | u8 | 音量修正 | 0 = 不做 |
| +0x7E | s8 | 循环基准 | 0 |
| +0x7F | u8 | 循环修正 | 0 |
| +0xBC | u32 | 额外头偏移 | 相对 0xBC |

**时间基**：所有等待计数都以 **44100 tick/秒** 为单位。这是 libvgm 写死的
（`RefreshTSRates()` 把 `_tsDiv` 写死 44100），与 SID 的 PHI2 无关。PHI2 只出现在
额外头的时钟表里，用来标明芯片实际工作频率。

把 0x24 填成 985248 是个典型错误：libvgm 会把它当 recordHz，在有该字段时做
倍速修正，播放速度直接快 22 倍。

## 3. 额外头

```
+0x00  u32  本结构长度（不含自己这 4 字节）= 0x0C
+0x04  u32  时钟表偏移（相对本字段）
+0x08  u32  音量表偏移（相对本字段）
```

### 3.1 时钟表

```
u8   count
重复 count 次： u8 type + u32 clock（小端），步长 5
```

| type | 含义 |
|---|---|
| 0x65 | SID #0（`$D400`） |
| 0x66 | SID #1（`$D420`） |
| 0x67 | SID #2（`$D440`） |

0x65–0x67 落在 libvgm `DEVID_*` 空间（0x00–0x29）之外，不与任何既有设备冲突。
时钟写 PHI2 频率（PAL 985248 / NTSC 1022730）。

### 3.2 音量表

```
u8 count
```

本格式写 `0`：SID 的输出幅度由芯片自己 4 位主音量决定，没有混音器增益可写。

## 4. 命令流

| 字节 | 长度 | 参数 | 含义 |
|---|---|---|---|
| `0x41` | 3B | `reg u8, val u8` | 写 SID #0 的寄存器 |
| `0x42` | 3B | 同上 | 写 SID #1 |
| `0x43` | 3B | 同上 | 写 SID #2 |
| `0x61` | 3B | `u16 n` | 等待 n tick |
| `0x66` | 1B | — | 流结束 |
| `0x67` | 7B+ | `66 type len32 data` | 数据块，预留给 digi/PCM |

三个写命令选 `0x41/0x42/0x43` 的原因：VGM 的命令表里 `0x41`–`0x4E` 是
"unknown, length 3"（`Cmd_unknown` 按长度跳过、不中止），形状正好是
`opcode + reg + data`。旧的 `0x69/0x6A/0x6B` 落在 VGM 已定义的
`0x69`（OKIM6295）上，会和别的设备撞车。

## 5. 循环点

主头 0x1C / 0x20 是结构性的，**当前录制器一律写 0**：`.sid` 文件和两个参考
播放器（libsidplayfp、WebSid）都不携带循环点，检测循环点属于另一个工具的
活，不属于录制。

写 0 的语义按 libvgm：`0x1C == 0` 表示"不循环"，播放器在 `0x66` 处停止。

## 6. GD3

```
"Gd3 " + u32 version(0x00000100) + u32 EOT长度(相对 0x0C) + 11 个 UTF-16LE 串
```

11 个串固定顺序：TITLE、TITLE-JPN、GAME、GAME-JPN、SYSTEM、SYSTEM-JPN、
ARTIST、ARTIST-JPN、DATE、ENCODED_BY、COMMENT。

本录制器填 TITLE（曲子名）、ARTIST（作者）、DATE（发布年）、ENCODED_BY
（`sidvgm 1.00`）、COMMENT（子曲 + 生成工具）。

## 7. 一个完整例子

`sidextract sid/Golden_Axe.sid ga.sidvgm --seconds 30`：

```
+0x000  53 76 67 6D          "Svgm"
+0x004  F5 D4 01 00          EOF = 0x1D4F5 (relative to 0x04)
+0x008  00 01 00 00          version 1.00
+0x014  53 D4 01 00          GD3 = 0x1D453 (relative to 0x14)
+0x018  40 2A 14 00          total ticks 1321408 (= 29.96 s)
+0x01C  00 00 00 00          no loop
+0x024  32 00 00 00          region 50 = PAL
+0x034  9F 00 00 00          data = 0xD3 (relative to 0x34)
+0x0BC  C0 00 00 00          extra header at 0xC0
+0x0C0  0C 00 00 00          extra header length
+0x0C4  0C 00 00 00          clock table at 0xCC (relative)
+0x0C8  12 00 00 00          volume table at 0xD2 (relative)
+0x0CC  01                   one chip
+0x0CD  65 C0 0B 0F 00       type 0x65 (SID0), clock 985248
+0x0D2  00                   empty volume table
+0x0D3  41 18 1F ...         SID0 $18 = 1F, then the rest of the stream
```

读回来 dump 一遍：

```
$ sidextract ga.sidvgm --dump --cmds 4
  version   : 1.00
  region    : 50 (PAL)
  tick rate : 44100 Hz (VGM fixed)
  ticks     : 1321408 (29.96 s)
  chips     : 1
    SID0  type 0x65 @ 985248 Hz
```

## 8. 与 `.sidreg` 的对比

| | `.sidreg` | `.sidvgm` |
|---|---|---|
| 形态 | 文本 `ms reg val` | 二进制，VGM 式头 + 命令流 + GD3 |
| 时间基 | 浮点毫秒（±0.1 µs 噪声） | 整数 tick（1/44100 s，可精确往返） |
| 元数据 | 无 | 标题/作者/年份/地区/芯片型号 |
| 循环点 | 无 | 头部可表达（现恒为 0） |
| 3 万条事件的体积 | ~430 KB | ~120 KB |
| 往返误差 | — | 最大 0.0114 ms（半个 tick） |

实测：10 首样例，`sid → sidvgm → sidreg` 与 `sid → sidreg` 的事件数、
寄存器/数据序列完全一致，最大时间偏差 0.0114 ms。

## 9. 读器校验清单

1. 前 4 字节 == `"Svgm"`；
2. EOF 偏移算出后不超出文件长度（超出则退化为文件长度，与 libvgm 一致）；
3. data 偏移落在 `[0xC0, EOF)` 之内；
4. GD3 偏移落在 `[0xC0, EOF)` 之内，否则视为无标签；
5. 命令流只认 0x41/0x42/0x43/0x61/0x66/0x67，遇到别的立即停止解析（不做
   "跳 N 字节继续"的猜测）；
6. 0x61 的 N 为 0 时只前进 3 字节，不能死循环；
7. 0x41–0x43 的参数不足 2 字节时视为文件截断。
