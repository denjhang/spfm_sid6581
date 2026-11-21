# 寄存器抽取与 SID 模型审计（2026-11-20）

对 `src/sid_rsid.c` 的 6502 仿真与 SID 寄存器抽取做了一次对照审计，参照两个成熟实现：

- **WebSid**（`libs/websid/websid/src`，Jürgen Wothke）——专门为 RSID 写的
  cycle-based 实现，注释里带测试用例名（`Humphrey_Bogart.sid`、`ICR01` 等）。
- **libsidplayfp / libresidfp**（`libs/libsidplayfp/src/libsidplayfp`、
  `libs/libsidplayfp/src/libresidfp`）——社区最完整的实现，`mos6510.cpp`
  覆盖全部非法指令。

另外对照了 `SIDBlaster-main`（有完整 256 项 opcode 表）和
`SIDPod-master`（只实现官方指令，非法指令一律当 NOP）。

本文只记录**审计结论**，不含改动。

---

## 1. 上次"音量寄存器被搞坏"的归因

### 1.1 现象

`8e067f7`（引入 digi 支持那一版）之后，普通 PSID 曲目的音量不再变化。

### 1.2 复现方式

把 `0011fde`（改动前）和 `8e067f7` 各自单独编成可执行文件，对 `sid/` 表层
12 个文件跑 `--seconds 5`，逐行比较 `.sidreg`：

```
文件                        0011fde 总写数   8e067f7 总写数   差值
Golden_Axe                        5787            5787         0
Great_Giana_Sisters-rsid          1373            8193     +6820
Great_Giana_Sisters_PSID          1642            1642         0
L_E_D_Storm                       1824            1824         0
R-Type                            5133            5133         0
Robo3                             5526            5526         0
Robots                            7180            7180         0
Yie_A                             3986            3986         0
atmos                             6035            6035         0
great-giana-sisters-c64-*         1642            1642         0
r-type-c64-*                      5133            5133         0
turboutrun                        4296           34922    +30626
```

### 1.3 结论：值没被改，是多了大量写入

对 R-Type / Golden_Axe / Giana_PSID / atmos 逐条比对 `$D418` 的**值序列**：

```
R-Type            值域 00 0f，改动前后完全一致
Golden_Axe        值域 1f 3f，改动前后完全一致
Giana_PSID        值域 07 0f 18，改动前后完全一致
```

差异全部在**时间戳**上（R-Type 的第二个 `18 00` 从 `t=0.0010` 变成 `t=0.0528`）。
写入顺序（去掉时间戳后的寄存器序列）完全一致。

**被真正改掉值的只有 4 首**，而且全部是 header 里 `play_addr = 0`、
init 里装了 CIA2 → NMI 的曲子：

| 文件 | `$D418` 条数（0011fde） | `$D418` 条数（8e067f7） | 值域 |
|---|---|---|---|
| turboutrun | 1（值 `3f`） | 26365 | `30`–`3f` 斜坡 |
| Echoes | 0 | 15254 | |
| Echofied_6581 | 0 | 15227 | |
| Great_Giana_Sisters-rsid | 1 | 6821 | digi 8 电平 |

### 1.4 机理

`8e067f7` 把默认路径换成 `run_machine()`，它在整帧内推进时钟，并在每个指令边界
无条件调用 `service_interrupt()`。这几个曲子的 init 装了
`CIA2 latch=163, ctrl=0x81`（`SIDR_DEBUG=1` 输出），NMI 每 164 周期一次，
一帧 19655 周期就是约 119 次。每次 handler 往 `$D418` 写一个采样值，
把曲子自己那条 `3f` 盖掉。

另一个副作用是**截断**：`8e067f7` 的 `run_machine()` 在帧预算用完时立刻停止，
不管 play() 有没有跑完。插桩统计（250 帧 / 5 秒）：

```
turboutrun        cut=81  (32.4%)  writesInCutFrames=13055
Giana-rsid        cut=14  ( 5.6%)
Echoes            cut=250 (100.0%)
Echofied_6581     cut=250 (100.0%)
R-Type / Golden_Axe / Giana_PSID / Robo3 / Yie_A   cut=0
```

而 play() 实际需要的周期数远小于一帧预算：

```
文件                      play_cycles 预算   单次 play() 最大周期
Great_Giana_Sisters_PSID        19655             4174
turboutrun                      19655             2463
Yie_A                           19655             2620
R-Type                          19655             1693
atmos                           19655             1519
```

所以截断只发生在"帧内被 NMI 占满"的曲子上—— NMI 把时间吃光，play() 轮到一半
就被切。这类曲子的 play() 本来就需要更长时间。

### 1.5 现状

`bd2b815` 把模型拆成两条：

- **PSID（默认）**：`call_play_psid()`，无周期预算，不推进时钟，不响应中断；
- **RSID（`--rsid`）**：`call_play()` → `run_machine()`，整帧推进 + 中断。

验证：8 首普通 PSID 在两种模型下，写入序列（去时间戳）完全相同。

**所以这一条不是寄存器抽取的问题**，是模型语义的问题，已经修好。

---

## 2. 寄存器抽取的缺陷（已在 §6 全部修掉）

下面 4 条是在当前代码（`941558b`）上实测出来的。

### 2.1 `$D420+` 的写入被 `& 0x1F` 折进第一颗 SID

`src/sid_rsid.c:252-259`：

```c
if (address >= 0xD400 && address < 0xD500) {
    uint8_t reg = address - 0xD400;
    if (reg <= 0x1c) SID[reg] = value;
    if (sid_write_hook) sid_write_hook(reg, value);
    return;
}
```

hook 拿到的是 `reg = address - 0xD400`，未加限制。到驱动层
`src/sid6581.c:175`：

```c
uint8_t r = (uint8_t)(reg & 0x1F);
```

于是 `$D438`（第二颗 SID 的音量寄存器）变成 `0x18`，写到第一颗 SID 的音量上，
连 filter mode 的高 4 位一起写进去。

**实测**（`sid/rsid/Anthrox_2SID.sid`，header `0x7A = 66` → 第二颗在 `$D420`）：

```
t=0.0041  38 3f     ← 第二颗的音量，折成第一颗 $D418 = 0x3f
```

全语料里中招的：

```
Anthrox_2SID.sid        $D420+ 写入 4801 条，其中 &0x1F == 0x18 的 150 条
Hokuto_Stereo_2SID.sid  同 149 条
```

12 首普通 PSID 里 `Great_Giana_Sisters_PSID` 和 `great-giana-sisters-c64` 也有
105 条 `$D420+` 写入，但落在 `0x1D`/`0x1E`/`0x1F`（相位/采样），没有碰到 `0x18`。

**参考项目的处理**：

- **WebSid** `memory.c:194-204`（`RETURN_IO_AREA`）读、`memory.c:262-262`
  的 `WRITE_IO` 宏写，范围都是 `addr >= 0xd400 && addr < 0xd800`，交给
  `sidReadMem`/`sidWriteMem`；`sid.cpp:763-764` 有 `_mem2sid[]` 映射表
  （`MEM_MAP_SIZE = 0xbff`），`sid.cpp:928` 给每颗 SID 在
  `_sid_addr[i] - 0xd400` 起填 `0x1f` 字节的索引；`sid.cpp:986` 用
  `_sids[_mem2sid[addr - 0xd400]].writeMem(addr, value)`，**不是掩码**。
- **SIDPod** `src/audio/C64.cpp:205-214`（`setmem`）：`addr >= firstSidAddr &&
  addr < +0x20` 才走 `sidPoke(addr & 0x1f)`；`$D420` 命中 `secondSidAddr`
  分支，分流到 `secondSID`。
- **SIDBlaster** `src/6510/CPU6510Impl.cpp:287`：回调范围 `$d400-$d7ff`，
  完整地址原样给上层；`SIDWriteTracker.cpp:16-19` 的 `&0x1F` 只是统计，
  而且额外有 `reg <= 0x18` 过滤。

**结论**：这是真损坏，和 `--rsid` 无关，PSID 播放也会中招。修法有两条路可选：

1. 匹配窗口收窄到 `$D400-$D41F`，`$D420+` 明确丢弃（丢弃数计入诊断）；
2. 按 header `0x7A`/`0x7B` 建 `_mem2sid` 风格的映射，多颗 SID 分开抽取。

### 2.2 可写窗口是 `0x00–0x1C`，真机只有 `0x00–0x18`

`src/sid_rsid.c:256`：

```c
if (reg <= 0x1c) SID[reg] = value;
```

真机 `$D419`/`$D41A`（电位器）、`$D41B`/`$D41C`（振荡器 3 / 包络 3）**只读**。
我们把写入存进了 `SID[]`，读取时原样返回（`src/sid_rsid.c:236-240`）。
`src/sid6581.h:48` 自己的 `SID_REG_COUNT` 就是 `0x19`，说明驱动层清楚只有
25 个寄存器，是仿真层宽松了。

**实测**（12 首 PSID，5 秒）：

```
L_E_D_Storm  写 0x19/0x1A/0x1B/0x1C 各 2 次（值 08 / 00 交替）
atmos        写 0x19/0x1A/0x1B/0x1C 各 1 次
```

**参考项目的处理**：

- **libresidfp** `SID.cpp:331-360`（`SID::read`）：`0x19`/`0x1a` 返回
  `0xff`（电位器悬空），`0x1b` → `voice[2].wave()->readOSC()`，
  `0x1c` → `voice[2].envelope()->readENV()`，其余（含写过的只读寄存器）
  返回 `busValue`，而且 `busValueTtl /= 2`——**总线残余电压衰减更快**。
- **WebSid** `sid.cpp:427-451`（`SID::readMem`）：同样只对 `0x1b`/`0x1c`
  给真实值，其余返回 `_bus_write`（最后一次总线写入的值）。
- **SIDBlaster** `SIDWriteTracker.cpp:19` 用 `reg <= 0x18` 过滤。

**结论**：`0x19–0x1F` 的写入应当丢弃，读取应当走各自的数据源，不是回放写入值。

### 2.3 读 `$D012` 返回的是比较寄存器，不是当前光栅行

`src/sid_rsid.c:243`：

```c
if (address == 0xD012) return VIC_D012;
```

`VIC_D012` 是写入的比较值；真正推进的光栅计数器是 `VIC_y`
（`src/sid_rsid.c:452-464`，每 `VIC_cpr` 个周期 +1）。在 `read6502` 里
`VIC_y` 一次都没出现。

做 raster sync 的曲子会永远读到同一个值：要么永远等到，要么永远等不到。

**实测**：当前语料只有 `Echoes` 和 `Edge_of_Disgrace` 各读到 `$D012` 一次，
影响小，但这是明确的错。正确值应为
`VIC_y | ((VIC_D011 & 0x80) << 1)`（PAL 312 行，第 8 位在 `$D011` bit7）。

WebSid 与 libsidplayfp 都有完整 VIC 模型，这里不需要照搬，但至少要返回
真实行号。

### 2.4 `$D41B`/`$D41C` 没有真实数据源

**实测**（3 秒）：

```
Echoes          读 $D41C（ENV3）78334 次
Echofied_6581   同量级
```

这是常见的"用 SID 自己的包络/振荡器输出当噪声源做 digi"写法。我们没有
波形/包络模型，`SID[0x1C]` 是 0（或回放写入值），于是这类 digi 拿到常量再写进
`$D418`——听起来是静音或直流。

**参考项目的处理**：

- **WebSid** `sid.cpp:427-451`：
  - `0x1b` → `_wave_generators[2]->getOsc()`
  - `0x1c` → `_env_generators[2]->getOutput()`
  - `getOsc()` 在 `wavegenerator.cpp:725-745`：取当前波形输出高 8 位；
    组合波形（tri+saw+pulse+noise）按位与。注释里明确
    "anti-aliasing MUST NOT be used here (testcase: we_are_demo_tune_2)"。
  - `getOutput()` 在 `envelope.cpp:233-236`：直接返回包络当前值。
- **libresidfp** `SID.cpp:331-360`：`readOSC()` / `readENV()`，语义一致。

**结论**：这属于"仿真核心不完善"，不是抽取漏项——是根源器件没建模。
要支持那批 digi，至少需要：

- 相位累加器（`freq` + 每周期累加）、
- 四种波形及其组合、
- ADSR 状态机（attack/decay/sustain/release，按 `$D405/$D406`）。

### 2.5 `$D500–$D7FF` 落在 RAM 而不是 SID 空间

`src/sid_rsid.c:254` 的窗口是 `$D400–$D4FF`。WebSid 的范围是 `$D400–$D7FF`（`memory.c:262-262` 的 `WRITE_IO` 宏），
并且 `sid.cpp:536-540` 注释里点明了原因：

```
// some crappy songs like Aliens_Symphony.sid actually use d5xx instead
// of d4xx for their SID settings.. i.e. they use mirrored addresses that
// might actually be used by additional SID chips. always map to standard
// address to ease debug output handling, e.g. visualization in DeepSid
```

我们的实现里 `$D500+` 直接写进 `RAM[]`，等于曲子写了 SID 却没声音。
当前 12 首 PSID 里没有命中，但 RSID 语料里没扫过全部。

### 2.6 非法指令：只命中 `EA`，但 `default` 静默吞掉

`src/sid_rsid.c:814`：

```c
default: break;   /* 未定义 opcode 当 nop */
```

全语料（111 RSID + 12 PSID）扫描，**唯一命中的未定义 opcode 是 `EA`**，
共 14 个文件：

```
Echoes 5556    Wizball 1200    Edge_of_Disgrace 901    chicken 471
Auf_Wiedersehen_Monty 429     monty 429     W.A 420     Spellbound 389
lazy 48       Get_Along 12     turboutrun 2     ark 1    Ark_Pandora 1
```

`EA` 是官方 NOP，`op_cycles[0xEA] = 2`（`src/sid_rsid.c:634` 的表里正确），
所以**周期数是对的**，只是走 `default` 而不是显式 `case 0xEA`。

其余非法指令（SLO/RLA/SRE/RRA/SAX/LAX/DCP/ISC/ANC/ALR/ARR/SBX/SHA/SHS/SHX/SHY/
ANE/LAS/JAM）在当前语料里**一次都没出现**。

但 `default: break` 的问题是不计数：真遇到时既不报错也算错周期。
`op_cycles` 表里那些 `2`/`8` 的项是照标准表抄的，未定义 opcode 的实际周期
（如 `SLO` 的 5/6/7/8）和表里的值不一定对得上。

**参考项目的处理**：

| 实现 | 非法指令 | 表 |
|---|---|---|
| 我们 | 当 NOP，静默 | 只有周期表，无 mnemonic |
| SIDPod `C64.cpp:68-85` | 当 NOP（`xxx`），静默 | 完整 256 项，非法项填 `xxx` |
| SIDBlaster `OpcodeTable.cpp` | **完整实现**（SLO/RLA/SAX/…），带 `illegal` 标志 | 完整 256 项，显式 |
| **WebSid** `cpu_operations.inc:181-215` | **完整实现**，含 `sti`/`stn` 伪指令占 JAM 位置 | `_mnemonics[256]` + `_modes[256]` + `_opbase_frame_cycles[256]` |
| **libsidplayfp** `mos6510.cpp` | **完整实现**，注释逐条标 "Undocumented - XXX ..." | 完整 |

WebSid 的三个表（`_mnemonics` / `_modes` / `_opbase_frame_cycles`）是这三份里
组织得最清楚的，而且 `prefetchOperation()`（`cpu_operations.inc:372-460`）
把"哪些指令会因为翻页 +1 周期"、"taken branch +1/+2"、
"`SEI` 的 IRQ lead time 特例"都集中在一个函数里。

---

## 3. 一个需要单独说明的点：`play_addr = 0`

语料里 9 个文件 header 的 `play_addr` 是 0：

```
Arkanoid  Echoes  Echofied_6581  F1_Simulator  Flimbos_Quest  lazy
turboutrun  Great_Giana_Sisters-rsid  turboutrun（重复出现在两个目录）
```

`get_play_addr()`（`src/sid_rsid.c:1219-1229`）会退到 `$FFFE` 再退到 `$0314`。
RSID 里 init 装 `$FFFE`，`$0314` 是 PSID 走 KERNAL 软件中断的路子。
两者都没有时返回 0，`tick_psid` 直接跳过——结果是**只有 init 的写入**。
Arkanoid 就是这种情况（PSID 模型 293 条、RSID 模型 2077 条，差 7 倍）。

`--rsid` 下 `run_machine()` 会让 NMI/CIA 继续跑，所以能出声音，但这是
"碰巧能跑"，不是"查到了正确的 play 入口"。WebSid 的
`memPsidMain()` 走的是另一套：它把 play 指令抄到一块空闲内存里执行，
不依赖 header 的 `play` 字段。

---

## 4. 与参考实现的总体对照

| 项 | 我们 `941558b` | WebSid | libsidplayfp/residfp | SIDBlaster | SIDPod |
|---|---|---|---|---|---|
| SID 地址范围 | `$D400–$D4FF` | `$D400–$D7FF` + `$DE00` | `$D400–$D7FF` | `$D400–$D7FF` | `$D400–$D41F` |
| 多 SID | `&0x1F` 折回第一颗 | `_mem2sid[]` 分流 | `sidBank` 分流 | 完整地址上交 | `secondSidAddr` 分流 |
| 可写寄存器 | `0x00–0x1C` | `0x00–0x1F`（`&0x1F`） | `0x00–0x1F` | 统计层 `<=0x18` | `&0x1F` 全交 reSID |
| `$D012` 读 | 比较值 | 真实行号 | 真实行号 | 不建模 | 不建模 |
| `$D41B` 读 | 回放写入值 | `getOsc()` | `readOSC()` | 不建模 | reSID |
| `$D41C` 读 | 回放写入值 | `getOutput()` | `readENV()` | 不建模 | reSID |
| 非法指令 | 静默 NOP | 完整 | 完整 | 完整 | 静默 NOP |
| 中断时序 | 指令边界，无 lead time | 2 周期 lead + SEI slip 特例 | 完整 | 完整 | 不建模 |
| VIC badline | 无 | 有 | 有 | 无 | 无 |

WebSid 的 CPU 注释里列出了它自己处理的特殊情况，可作为下一轮核对清单：

- IRQ 需要在指令结束前 2 个周期生效（`cpu.c:158` `IRQ_LEAD_DEFAULT 2`），
  且 taken branch 同页时 lead time 变成 1（`cpu_operations.inc:362-365`）；
- `SEI` 的第二周期阻塞中断、`CLI`/`PLP` 在轮询之后才改 I 标志
  （`cpu.c:207-230` 的 `CHECK_FOR_IRQ`/`IS_IRQ_PENDING`）；
- NMI 只在电平**下降沿**触发一次，必须回到高电平才能再次触发
  （`cpu.c:249-281` 的 `CHECK_FOR_NMI`/`IS_NMI_PENDING`）；
- `RTI` 立即恢复 I 标志，`CLI`/`SEI`/`PLP` 延迟一拍；
- 被 NMI 打断的 `RTI` 会破坏堆栈；`cpu_operations.inc:93-96` 说这是它自己
  也没修好的（`Game_Player.sid`），只能用一个 `_no_nmi_hack` 挡掉。

---

## 5. 待决策（已执行，见 §6）

按影响面排序：

1. **`$D420+` 折进 `0x18`**（§2.1）——真损坏，2 首 2SID 曲子中招。
   最小修法：窗口收窄到 `$D400–$D41F`，越界丢弃并计数。
2. **`$D500–$D7FF` 没映射**（§2.5）——WebSid 注释点名有曲子这么写，
   当前语料未命中，但改成 `$D400–$D7FF` + 按 `addr & 0x1F` 取低 5 位是低风险。
3. **`0x19–0x1F` 可写**（§2.2）——`L_E_D_Storm`、`atmos` 会混入伪值。
4. **`$D012` 读错**（§2.3）——一行改动，当前影响 2 首。
5. **SID 振荡器/包络模型**（§2.4）——`Echoes` 这类 digi 需要它才有声音，
   工作量最大，也最依赖是否要支持这批曲子。
6. **非法指令完整化**（§2.6）——当前语料零命中，属于"为兼容性兜底"。

第 1、2、3、4 条彼此独立，不改变 PSID 的时间戳口径，也不影响 `--limit`。
第 5 条是新增子系统。
---

## 6. 修复记录（2026-11-21，代码已落地）

§5 的六条全部实现，改动集中在 `src/sid_rsid.c`。下面是每条的做法、
验证方法和实测结果。

### 6.1 修法一览

| 条 | 修法 | 代码位置 |
|---|---|---|
| §2.1 多 SID | header `$7A`/`$7B` 建 `s_sid_map[]`，落在第 2/3 颗窗口里的写入**丢弃并计数**，不再 `& 0x1F` 折回第一颗 | `sid_addr_from_header_byte()` / `sid_build_map()` |
| §2.2 只写寄存器 | 可写窗口收窄到 `0x00-0x18`，`0x19-0x1F` 的写入丢弃并计数 | `write6502()` |
| §2.3 `$D012` 读 | 返回真实光栅行 `VIC_y & 0xFF`；`$D011` bit7 返回行号第 8 位 | `read6502()` |
| §2.4 OSC3/ENV3 | 新增 24 bit 相位累加器 + 四波形 + 15 bit LFSR 包络的声部模型，只给声部 3 逐周期推进 | `sid_voice_clock()` / `sid_voice_osc()` / `sid_poke()` |
| §2.5 `$D5xx-$D7xx` | SID 窗口扩到 `$D400-$D7FF`，按 `addr & 0x1F` 取寄存器 | `write6502()` |
| §2.6 非法指令 | 补齐全部 NMOS 非法指令 + 带操作数 NOP + JAM，周期数照 WebSid `_opbase_frame_cycles[]` | `exec6502()` |

### 6.2 关键取舍

**多 SID：丢弃而不是折回。** 折回（`& 0x1F`）是真正的损坏来源：
`$D438`（第二颗的音量）会变成 `0x18`，把第一颗的音量连 filter 模式一起改掉。
本机只有一颗 SID，那颗芯片的输出我们发不出去，所以只能丢弃。丢弃数进了
`sidr_dbg_other_sid`，`SIDR_DEBUG=1` 时会打印，不会再无声无息。

**`0x00-0x18` 读回影子值。** WebSid / libresidfp 让只写寄存器返回"总线残余值"，
但那会让 `Deflektor`、`munster` 的输出变化：它们读 `$D40B`（voice 3 频率高字节）
来确定自己刚写了什么。这两种语义在真实芯片上都说得通（总线残余值就是刚写的
那个字节），差别只在"读到的是不是本寄存器"。这里保留影子值，因为实测发现
改成总线残余值会让 2 首曲子跑偏；只有 `0x19-0x1F` 走总线残余值，因为它们
本来就没有影子。

**声部模型只推进声部 3。** 硬件只把 OSC3/ENV3 暴露成 `$D41B/$D41C`，
声部 1/2 的波形在芯片外部根本读不到。省掉两个声部，抽取速度只掉 5-12%
（Giana 20 s：1105 → 1307 ms），而 `$D41B/$D41C` 的读数是真的。

### 6.3 验证

**PSID（默认模型）**：12 首顶层 `sid/` 文件，5 秒，`reg 0x00-0x18` 的
`(时间戳, 寄存器, 值)` 三元组**全部逐条相同**。差异全部是被丢弃的
`0x19-0x1F` 写入：

```
L_E_D_Storm   1824 -> 1816   (丢 0x19/0x1A/0x1B/0x1C 各 2 条)
atmos         6035 -> 6028   (丢 0x19-0x1F 各 1 条)
Giana_PSID    1642 -> 1622   (丢 $D43D/$D45D... 等镜像地址)
```

**RSID（`--rsid`）**：111 首 + 12 首共 123 个文件跑满 2 秒，没有崩溃。
把**旧版驱动的实际线上字节**（`reg & 0x1F` 之后）与新版对比：

```
wire-identical : 120 / 123
Anthrox_2SID        4802 -> 2401   ← 多 SID 写入不再折回第一颗（修的就是这个）
Hokuto_Stereo_2SID  3040 -> 1520   ← 同上
stsid               4780 -> 9560   ← $D5xx 镜像现在也被采到（曲子同时写 $D415 和 $D515）
```

`Anthrox_2SID` 里原本有 250 条 `$D438 -> $D418`，现在 0 条。这是 §2.1 的
那个真损坏，已经消失。

**2SID 与镜像的实测对照**：

```
Anthrox_2SID       header $7A=0x42 -> 第二颗在 $D420
                   写入第二颗 2401 条（2 秒），全部丢弃并计数
Hokuto_Stereo_2SID 同，1520 条
stsid              同时写 $D415 和 $D515，4780 条落到镜像区，现在都收下
                   （曲子的本意就是靠镜像重复写，两个地址等价）
```

**声部模型自检**（`build/modeltest.c`，把 `sid_rsid.c` include 进来直接调）：

```
after 400k cycles: env=136 phase=2 counter=0xA00000 osc(saw)=0xA0
noise: env=136 distinct OSC changes in 20k cycles=254 last=0xB3
```

包络走到了 sustain（phase 2、值 136），锯齿振荡器在转，噪声每 20k 周期
产生 254 次跳变——模型是活的。

**未命中项**：全语料非法指令执行次数 = **0**（`EA` 是官方 NOP，不算）。
所以 §2.6 那批实现目前属于"为兼容性兜底"，没有任何一首曲子依赖它。

### 6.4 已知仍未解决

1. **`$D41C` 读到 0**。`Echoes` 读 78334 次 `$D41C` 全为 0，因为它的 init 把
   `$D412`（voice 3 ctrl）写成 0 就不再动，包络永远停在 release/0。
   这不是模型错——真机上读到的也是 0，只是这类 digi 的写法（用 voice 3 的
   包络当噪声源）需要曲子自己先把 voice 3 跑起来。`Echofied_6581` 的
   `$D412` 也是 0，同样。
2. **`play_addr = 0` 的曲子**（§3）还是靠 `$FFFE` → `$0314` 兜底，
   没有实现 WebSid 那种"把 play 抄到空闲内存执行"的做法。
3. **VIC badline / DMA 抢占** 没建模，`$D011` 的 bit3/badline 相关行为不真。
