# MOS 6581 SID 官方数据手册文本·中文版（m64.io Chiptune Wiki 整理版）

> 原文: <https://m64.io/chiptune-wiki/wiki/s-mos-6581-datasheet>  
> 页面日期: 2026-08-30  抓取日期: 2026-11-19  
> 本文件为 [mos6581_datasheet_m64.md](mos6581_datasheet_m64.md) 的完整中文翻译；寄存器名、位名等标识符保留英文。

制造商这份十二页的 [SID](https://m64.io/chiptune-wiki/wiki/sid) 规格书：模块框图、29 个寄存器映射、每个寄存器位的逐段说明、包络率表、滤波器、引脚与电气参数、音阶表与省内存算法，以及一篇关于 ADSR 包络的短文和五个成品乐器包络。《C64 程序员参考手册》将其作为附录 O 转载（[s-c64-prg-chapter-4-sound](https://m64.io/chiptune-wiki/wiki/s-c64-prg-chapter-4-sound)）；SID-Wizard 一书引用了其中若干句子（[s-witchmaster-creating-chiptunes-with-sid-wizard](https://m64.io/chiptune-wiki/wiki/s-witchmaster-creating-chiptunes-with-sid-wizard)）。扫描版 PDF 的文字层在寄存器表和表 2 的十六进制列有 OCR 噪声；下文数字已对照 PRG 手册中的同一张表核实。

## 关键要点

- 概念与特性：一颗"三声部电子音乐合成器/音效发生器"；振荡器 0–4 kHz，各四种波形，三个包络发生器、"指数型响应"，起音(attack) 2 ms–8 s，衰减(decay)/释音(release) 6 ms–24 s，保持(sustain) 0–峰值；振荡器同步(sync)、环形调制(ring modulation)、可编程滤波器(30 Hz–12 kHz，12 dB/倍频程，低通/带通/高通及陷波，可变谐振)、总音量、两路 A/D 电位器输入、随机数/调制发生器、外部音频输入。每个声部 = 音准振荡器/波形发生器 + 包络发生器 + 幅度调制器；滤波器用于"减法合成"。
- 寄存器：29 个 8 位寄存器，声部和滤波部分只写，POTX/POTY/OSC3/ENV3 只读；五根地址线可寻址 32 个位置，三个空位忽略写入、读出无效数据。声部 1 在 $00–$06（Freq Lo/Hi、PW Lo/Hi、Control、Attack/Decay、Sustain/Release），声部 2 在 $07–$0D，声部 3 在 $0E–$14，滤波器 $15 FC Lo（位 0–2）、$16 FC Hi、$17 RES/Filt（高半字节为谐振；Filt EX、Filt 3、2、1）、$18 Mode/Vol（3 OFF、HP、BP、LP；低半字节为音量）、$19/$1A POTX/POTY、$1B OSC3/Random、$1C ENV3。
- 频率：Fout = Fn × Fclk / 16777216 Hz；1.0 MHz 时钟下 Fout = Fn × 0.0596 Hz；16 位分辨率"允许从一音滑到另一音（滑音）而听不出频率台阶"。
- 脉宽：12 位（PW HI 的位 4–7 不用）；PWout = PWn / 40.95 %；0 或 4095 = 恒定直流，2048（$800）= 方波；必须选中脉冲波形脉宽才起作用；扫过脉宽"产生动态的'相位移动'感"，在多个脉宽之间跳变可产生"有趣的谐波序列"。
- 控制寄存器的位：0 GATE（置位 → 起音/衰减/保持，清零 → 释音；幅度完全由包络决定，所以必须选一个波形，但要静音不必取消波形选择）、1 SYNC（振荡器 1 同步到振荡器 3；osc 3 频率必须非零，"最好更低"）、2 RING MOD（用 1 与 3 的"环形调制"组合替换 osc 1 的三角波，"产生钟声/锣声的非谐波泛音结构"；必须选三角波、osc 3 非零；声部 3 的其他参数一概无关）、3 TEST（复位并把振荡器锁在零，复位噪声输出，把脉冲钳在直流电平；可用"把振荡器 1 同步到外部事件"）、4 三角波（"柔和、如长笛"）、5 锯齿波（"明亮、铜管味"）、6 脉冲（"从明亮空洞的方波到带鼻音尖锐的脉冲"）、7 噪声（"从低沉轰鸣到嘶嘶白噪声"，用于爆炸、风、海浪、"军鼓与镲"）。声部 2 的 SYNC/RING 用振荡器 1，声部 3 的用振荡器 2。
- 波形不可叠加：同时选多个是"逻辑与(AND)"；"噪声开着时再选任何其他波形，噪声输出可能'锁死'"，直到 TEST 位或 RES 引脚复位才恢复。
- 包络：表 2 —— 起音 2、8、16、24、38、56、68、80、100、250、500、800 ms、1、3、5、8 s；衰减/释音 6、24、48、72、114、168、204、240、300、750 ms、1.5、2.4、3、9、15、24 s（取值 0–F）；速率按 1.0 MHz Ø2 时钟给出 —— "其他 Ø2 频率把速率乘以 1 MHz/Ø2"；起音时间是零到峰值，衰减/释音时间是峰值到零。保持：16 个线性级，8 = 半峰值。释音速率等于衰减速率。"包络发生器的循环可以在任意时刻经 GATE 位改变"：起音途中清零则从当前幅度开始释音，释音途中重新置位则从当前幅度开始新的起音 —— "用实时软件控制出复杂的幅度包络"。
- 齐奏(Unison)：多声部合用并"对每个振荡器轻微失谐，或按音程调谐"，得到"丰满、有生气的声音"。
- 滤波器：11 位截止频率（FC Lo 的位 3–7 不用），配推荐的 2200 pF 电容时约 30 Hz–10 kHz（引脚说明写 30 Hz–12 kHz，并给出 FCmax = 2.6E-5 / C，范围向下延伸"约 9 个倍频程"）；谐振 16 级线性；Filt 1/2/3/EX 决定哪些声部与外部输入进滤波；LP 通过截止以下、以上按 12 dB/倍频程衰减（"厚实饱满"），BP 6 dB/倍频程（"单薄空灵"），HP 12 dB/倍频程（"尖薄、带嗡声"）；模式可叠加 —— LP + HP = 陷波；"滤波器或许是 SID 中最重要的部分……最好的效果来自实时改变截止频率"。3 OFF（配合 Filt 3 = 0）把声部 3 从输出断开，用于静音调制。
- 音量：16 级线性；"可用于做动态音量效果，例如颤音(Tremolo)"；"必须选择非零的音量"。
- 读回：POTX/POTY 每 512 个 Ø2 周期更新；OSC3 返回振荡器 3 的高 8 位（锯齿波数 0–255，三角波上下往返，脉冲在 0 和 255 间跳，噪声 = 随机数）—— 是个调制源：锯齿波加到别的振荡器频率上 = 警笛，噪声加到滤波截止上 = "采样保持"，约 7 Hz 的三角波按比例加到别的振荡器频率上 = 颤音；ENV3 需要声部 3 已门控，把"谐波包络、WAH WAH"加到截止频率，把"移相(phaser)"加到频率上。
- 引脚与电气：RES 拉低 ≥ 10 个周期清零所有寄存器；Ø2 标称 1.0 MHz，驱动振荡器、包络与总线传输；地线与 +5 V 分开走线"以减小数字噪声"；EXT IN（约 100 kΩ，6 V 直流偏置，≤ 3 V 峰峰，需隔直耦合，不经滤波时增益为 1）可把多片 SID 菊花链相连；AUDIO OUT 在 6 V 直流偏置上约 3 V 峰峰，需 1 kΩ 负载；Vdd 12 V，Vcc 5 V，约 600 mW。
- 附录 A：1.0 MHz 时钟下的 96 项十二平均律表（C0 = 274 = $0112 … A7 = 59056 = $E6B0；B7 = 66288 超出 16 位）；省内存方法 —— 只留最高八度的 12 个值（C7–B7），每降一个八度右移一位，配一个"低位=半音、高位=八度"的音符字节。
- 附录 B —— 把包络当乐器（A/D/S/R 值与时间）：弦乐/铜管/木管 A 8 A 9（起音 500 ms、衰减 300 ms、释音 750 ms）；鼓/镲 0 9 0 9（"无论 GATE 何时清零都在衰减"）；钢琴/羽管键琴 0 9 0 0（按住时缓降，一放就断）；风琴 0 0 F 0；"倒放"-A 0 F 3（慢起音、急止）；"SID 真正的威力在于创造原创音色，而不是模拟"。

## 实用收获

追踪器(tracker)乐器页抽象掉的一切，用寄存器语言说清楚：毫秒计的包络表、保持作为线性比例、gate/sync/ring/test 的确切含义、噪声加别的波形为何静音、滤波器的斜率与模式，以及官方认可的调制技巧（OSC3/ENV3 读回）。附录 B 的五条包络就是最初的 SID 乐器预设。

## 金句摘录

> "滤波器或许是 SID 中最重要的部分，它通过减法合成实现复杂音色的生成。"
> "噪声开着时再选任何其他波形，噪声输出可能'锁死'。"

## 相关性

SID 芯片的一手硬件资料；证实 wiki 上已有的 [adsr-envelope](https://m64.io/chiptune-wiki/wiki/adsr-envelope) 的 ADSR 时间表（转引自 SID-Wizard 一书）、[ring-modulation-and-sync](https://m64.io/chiptune-wiki/wiki/ring-modulation-and-sync) 的同步/环形调制规则，以及脉宽与滤波公式；此前 wiki 引用数据手册都是二手转述。

## 涉及页面

· [filter-programming](https://m64.io/chiptune-wiki/wiki/filter-programming) · [pulse-width-modulation](https://m64.io/chiptune-wiki/wiki/pulse-width-modulation) · [vibrato](https://m64.io/chiptune-wiki/wiki/vibrato) · [sid-player-routine](https://m64.io/chiptune-wiki/wiki/sid-player-routine) · [instrument-design](https://m64.io/chiptune-wiki/wiki/instrument-design) · [detune](https://m64.io/chiptune-wiki/wiki/detune) · [commodore-64](https://m64.io/chiptune-wiki/wiki/commodore-64) · [c64-programmers-reference-guide](https://m64.io/chiptune-wiki/wiki/c64-programmers-reference-guide) · [sidechain-pump](https://m64.io/chiptune-wiki/wiki/sidechain-pump) · [sound-effects](https://m64.io/chiptune-wiki/wiki/sound-effects) · [levels-and-headroom](https://m64.io/chiptune-wiki/wiki/levels-and-headroom)
