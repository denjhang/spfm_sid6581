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

---

## 7. 第二轮：多 SID、长度/子目，以及 §6.4 里那三件事

（原 §7.3 的循环点检测在后续一轮里整块删除了，理由见本节 7.3。）

### 7.1 多 SID 从"丢弃"变成"分颗捕获 + 分槽播放"

§6.1 那一轮的处理是**丢弃**第 2/3 颗 SID 的写入，因为当时只有一颗芯片、
而且折回第一颗是真损坏。这一轮把中间层补上了：

- `sid_event_t` 加了 `sid` 字段（0/1/2），每条写入带上它属于哪颗芯片；
- `.sidreg` 文件第 4 字段只在 `sid != 0` 时写出，**单芯片曲子的文件格式
  一个字都没变**，老缓存继续可用；
- 解调器（`--limit`）的"同格覆盖 / 重复值丢弃"状态改成**每颗一份**，
  否则两颗芯片的音量写会互相删掉对方的采样；
- `sidplay` 新增 `--slot2 N` / `--slot3 N`，把第 2/3 颗放到指定 SPFM 槽。
  没给槽时那颗的写入仍然被丢弃（只有一颗硬件，无处可发），启动时会打印
  `2 SIDs in the file: chip 0 on slot 3, chip 1 on slot -1 (not played - use --slot2)`。

实测（`Anthrox_2SID.sid`，header `$7A=0x42` → 第二颗在 `$D420`）：

```
改前（§6）：6 001 条，第二颗全部丢弃
改后      ：4 802 条，其中 2 401 条带 sid=1
启动打印  ：SIDs    : 2  #0 at $D400  #1 at $D420
```

**本机只有一颗 SID，所以双芯片的声音没法验证**；能验证的是"写入被正确
分流到两路"这件事，以及单芯片曲子的输出一字未变（见 §7.5）。

### 7.2 曲目长度：Songlengths.md5 + SSL

新增 `src/sidmeta.c`，两个来源都实现，顺序是"先本地 SSL，再 HVSC 数据库"：

| 来源 | 格式 | 用途 |
|---|---|---|
| `SONGLENGTHS/Name.ssl`（.sid 同目录） | 每子目 2 字节 BCD，分在前秒在后 | 本地覆盖、C64 播放器用的那份 |
| `Songlengths.md5` | INI，`<文件MD5>=mm:ss[.SSS] ...` 空格分隔 | HVSC #71 起的主格式 |

MD5 是**整个文件**的哈希（含 header），不是旧格式那种挑字段算的——这也
是 HVSC 从 #71 起唯一提供的形式。实测：

```
Robots.sid      md5 cfa8a19d…  →  subsong 1 = 0:52.403
Golden_Axe.sid  md5 8ace7ad8…  →  subsong 1 = 4:15, subsong 2 = 0:27
R-Type.sid      md5 492a9640…  →  subsong 1 = 2:11, 2 = 0:36, 3 = 0:26
```

`sidextract --info --db Songlengths.md5` 直接打印；`sidplay --length auto --db …`
播放到该长度自动停。

### 7.3 循环点：不做，按参考播放器的口径处理

第一版这里做过一个 RAM 指纹检测（每帧对状态区算 FNV-1a 64，同状态出现三次且
周期一致才认定），Golden_Axe 报 1.995 s、`[0,L)` 与 `[L,2L)` 的 2306 条寄存器
事件全同，看起来是对的。**这一版把它整块删掉了**，原因是查了参考实现之后发现
这个能力在任何一方都不存在，自己造一个会带来三类假值风险（详见下面的证据）。

**证据一：文件里没有循环点字段。** libsidplayfp 的 `psidHeader`
（`src/sidtune/PSID.cpp:53-70`）字段是封闭的：id/version/data/load/init/play/
songs/start/speed/name/author/released/flags/relocStartPage/relocPages/
sidChipBase2/sidChipBase3；HVSC #83 的 `SID_file_format.txt` 逐字段列到 +7B，
version 只有 1/2/3/4。+7A/+7B 是第 2/3 颗 SID 的地址，不是循环。WebSid 的
v4E 多 SID 扩展（`docs/SID_file_format+.txt`）也只加了 nSidFlags 和声道。

**证据二：Songlengths 只有时长。** `md5=mm:ss[.SSS] ...`，子目一节。旧格式的
`(G)(M)(Z)(B)` 是终止条件（gate 全关 / 主音量归零 / 三声部静音 / 内存乱写估的
时间戳），不是循环点（`Songlengths.faq:134-141`）。SSL 更简单，2 字节 BCD。

**证据三：两个引擎都不做检测。**
- libsidplayfp：`src/` 全树 `loop` 只命中滤波器 biquad 注释和 exSID 的延迟常量。
  `SidTuneInfo.h` 的公开接口没有任何 loop 项；`SidDatabase` 只有 `length()` /
  `lengthMs()`。CLI 的 `-ol` 是**歌单重复**（`args.cpp:255-270` 设
  `m_track.loop`，`player.cpp:1189-1206` 在 `m_timer.stop` 到点时
  `playerRestart` 从曲子开头重播），默认 `m_timer.length = 0; // FOREVER`。
- WebSid：`src/*.c` 里只剩 `_skip_silence_loop`（`Adapter.cpp:108/217`），那是
  每 chunk 最多跳 10 帧等 `SID::isAudible()`，躲开 100 秒才出声的曲子，与循环
  无关。前端 `timeout` 是 URL 参数 `file.sid;track;timeout` 交给
  `ScriptNodePlayer.setPlaybackTimeout` 的墙钟闹钟。
- 仅有的“循环点”出现在转换器里：`sid2vgm` 的 `VGMConverter.cpp:189-192`
  `// Set loop point at 2 seconds (after intro)`，固定 2 秒占位。

**证据四：HVSC 的时长本身就是循环点的人肉近似。** `STIL.faq` 定义 TIMESTAMP
时写的是 `(X:XX)` 表示“到下一个 TIMESTAMP，**或到曲子结尾（即开始循环时）**”
——作者手里也没有那个时间戳，只能靠听。

**现在的行为**（与 libsidplayfp 对齐）：`--length auto` + `--db` 时播到数据库
长度自动停；没有数据库条目、或没给 `--length` 时一直播到按键。`--length loop`
改为直接报错：

```
--length loop: a .sid has no loop point; use --length auto with --db
```

`sid_song_info_t` 里 `loop_ms` 字段、`loop_check()` / `loop_ms_from()` /
`loop_set_range()` / `loop_reset()` 以及每帧一次的调用全部删除，
`sidextract` 不再打印 `loop :` 行。

### 7.4 子目名称：STIL

STIL 的结构（HVSC #83 实测）：`/路径/Name.sid` 顶格以 `/` 开头，`(#N)` 也
顶格开启子目块，`TITLE:`/`ARTIST:` 缩进 1–2 格，`COMMENT:` 换行继续缩进更深。

两个坑：

1. **路径对不上**。STIL 的键是 HVSC 相对路径（`/MUSICIANS/H/Hubbard_Rob/
   Commando.sid`），而调用方可能只有 `sid/rsid/Commando.sid`。所以按**路径
   分量从后往前比**：文件名必须相同，前面有多少层目录就比多少层，短的那边
   比完即止。
2. **重名**。HVSC 里有 ~50 个同名文件（11 个 `R-Type.sid`）。所以不是"第一个
   匹配就用"，而是给每个候选打分（匹配到的路径分量数），**取最高分**。

实测：

```
Robots.sid       → 'The Robots [from The Man-Machine]' / Kraftwerk
Golden_Axe.sid   → subsong 1 'Death Adder [from the arcade game Golden Axe]'
                   subsong 2 'Character Select [...]'
R-Type.sid       → 文件名匹配到 11 个，但目录上下文不够 → 不猜，不报
```

最后一条是有意的：`sid/R-Type.sid` 没有任何目录线索，11 个候选并列，
宁可不说也不要给错。真放在 HVSC 树里时目录分量会把它唯一确定下来。

### 7.5 §6.4 里那三件事

**(1) 波形"悬空"（floating waveform）**。波形选择位从非零变成零时，模拟输出
并不立刻归零，而是保持上一个值约 3M 周期（WebSid `NULL_FLOAT_DURATION`）。
`Echoes` / `Echofied_6581` 先写 `$D412 = 0` 再读 `$D41B`，差别就在这里。
已实现（`sid_voice_t.float_out / float_until`）。

**但它没有让这两首出声音**，而原因查清了、不是模型的问题：
`Echofied_6581` 的 `$D412` 全程只写过一次 `0`（`$36BB: 8D 12 D4`），
`Echoes` 的 `$D412` 在 digi 路径上写的是 `A5 22 25 3D`（一个算出来的值），
但它的 `$D41B`/`$D41C` 读数仍然不随曲子变化。真机上这两首的 digi 声音
来自**声部 3 的包络/振荡器**，而这需要曲子自己先把声部 3 跑起来。
我们把 `$D41B` 非零读数的计数器加上了（`SIDR_DEBUG` 会打印），当前是 0。

**(2) `$0314` 中断入口**。真机 `$FFFE` 指向 KERNAL 的 ROM 例程，它先
`PHA/TXA/PHA/TYA/PHA` 再跳 `$0314`——所以装 `$0314` 的曲子写的是
"三次 push 之**后**"的地址。我们直接跳 `$0314` 就少推了三个字节，每次
中断都把栈错位。现在按 `irq_jump()` 里的判断补上：**只有当 `$FFFE` 没被
曲子改写（即走 KERNAL 路径）时才补 push**；曲子自己写 `$FFFE` 的（RSID
常见）保持原样。

效果：`Echoes` 的寄存器总数从 15 256 变成 14 391（`$D40B` 从 5 063 变
4 197），`turboutrun` 的写入值多重集完全不变、只有个别时间戳挪了 1 帧以内。
`Giana` / `R-Type` 等 92/111 首 RSID 逐字节相同。

**(3) VIC badline**。PAL 每帧 25 条 badline（行号 `$30–$F7` 且低 3 位等于
`$D011` 的低 3 位），每条抢走 40 个周期，即约 5% 的 CPU。已实现为
`run_machine()` 里的 `s_vic_stall` 计数器：被抢的周期照常推进时钟和 CIA，
只是不能执行 6502 指令。

### 7.6 回归

| 项目 | 结果 |
|---|---|
| 12 首顶层 PSID，5 秒，`0x00-0x18` | **12/12 逐条相同**（与 §6 提交时对比） |
| 111 首 RSID，2 秒 | 92/111 逐条相同；其余差异见下 |
| 全部 123 个文件 3 秒 `--rsid` | 零崩溃 |
| 全部输出时间戳 | 单调，无回退 |

RSID 差异分三类：

- **多 SID 分颗**（`Anthrox_2SID` 2401→4802、`Hokuto_Stereo_2SID` 1520→3040）：
  有意为之，第二颗的写入现在被捕获。
- **只有时间戳变**（`Antics`/`Bombo`/`Bulldog`/`Echofied_6581`/`Hyper_Sports`/
  `Krakout`/`Last_Ninja`/`Times_of_Lore`/`aztec`/`huey`/`ark` 等）：badline
  抢周期让指令串相位挪了一点，写入值和顺序不变。`turboutrun` 的
  (寄存器,值) 多重集**完全相同**。
- **条数变了**（`Echoes` 15256→14391）：`$0314` 的栈平衡修正让中断不再
  泄漏栈空间，NMI 次数变化，`$D40B` 的写入随之减少。这是修好的表现。

### 7.7 还没做的

1. **多 SID 只能"分流"，不能"听"**：本机一颗芯片，第 2 颗的写入发不出去。
   要么加第二颗 SID 的硬件，要么接受 `--slot2` 只是把数据准备好。
2. **`Echoes` 那类 digi 仍然无声**：需要曲子自己把声部 3 的包络跑起来，
   我们只保证读到的是真实值（0 就是 0）。要进一步，得实现"声部 3 的
   ADSR 由 SID 自己驱动而不是靠 $D412 的 gate"——那是仿真器的另一层。
3. **循环点不做**（见 §7.3）：文件里没有，参考播放器也不检测。播多久只由
   Songlengths 数据库或用户按键决定。
4. **STIL 只取 TITLE/ARTIST**，`COMMENT` / `NAME` / `AUTHOR` 没解析。
