# MDPlayer 真实芯片写入链路考察

目的：SID 的 digi 鼓（`$D418` 音量寄存器高速写入）在 SPFM 上送不出去，想从 MDPlayer
真实芯片（SCCI / GIMIC）的做法里找可借鉴的节流策略。

考察对象：`E:\working\vscode-projects\Reference_Project\vgm_libs\MDPlayer-STBL449`。
**只看真实硬件路径**（`ChipRegister.cs` 的 `sc*[chipID].SetRegister` 转发、
`RealChip.cs` 的 `RScciSoundChip`、`Driver/vgm.cs` 的调用点）。
仿真器（MDSound）源码不在本文范围内。

---

## 1. 前提：这个仓库里没有 SPFM 代码

全树搜 `spfm` 只命中三处，都是文档/资源：

```
README.md
MDPlayer\README_EN.md
MDPlayer\MDPlayerx64\Properties\Resources.resx
```

`NScci-master`、`SCCISharp-master` 里也搜不到。
所以 MDPlayer 对 SPFM 只有"名字出现过"，真正的 SPFM 写入在 SCCI 的 DLL 里，
本仓库看不到。下文所有结论都只针对 SCCI / GIMIC 那条路。

## 2. 真实芯片的写入函数只有一层

`RealChip.cs:805` `class RScciSoundChip : RSoundChip`，全部实现是转发：

| 成员 | 实现 | 作用 |
|---|---|---|
| `SetRegister(adr,dat)` | `realChip.setRegister(adr,dat)` | 寄存器写入，只转发，自身不缓冲 |
| `GetRegister(adr)` | `realChip.getRegister(adr)` | 读回 |
| `IsBufferEmpty()` | `realChip.isBufferEmpty()` | 查询接口缓冲是否排空 |
| `SetMasterClock` | 直接返回当前时钟 | SCCI 不允许改时钟 |
| `SetSSGVolume` | 空函数 | SCCI 不支持 |

`RealChip.cs:211` `SendData()` → `nScci.NSoundInterfaceManager_.sendData()`：
把已排队的写入**一次性批量推出**。
`RealChip.cs:229` `WaitOPNADPCMData()`：`sendData()` 之后
`while(!isBufferEmpty()) Thread.Sleep(0)`，即**等硬件消费完**。

`RSoundChip` 基类（`RealChip.cs:758`）一共只有
`Init / SetRegister / GetRegister / IsBufferEmpty / SetMasterClock / SetSSGVolume`
六个虚函数。**没有"降采样"、没有"丢点"这类接口**——上层想节流只能靠调用时机。

## 3. 四颗芯片的 PSG 写入链（AY8910 / YM2149 / YM2203 / YM2608）

四者同构，差别只在地址编码：

```
VGM 命令
  ├ 0x50 / 0xA0  → VcAY8910()        Driver/vgm.cs:589
  ├ 0x55 / 0xA5  → VcYM2203()        Driver/vgm.cs:676
  └ 0x56/0x57/0xA6/0xA7 → VcYM2608Port0/1()  Driver/vgm.cs:682
        ↓
set{AY8910,YM2203,YM2608}Register(chipID, [port,] addr, data, model)
        ChipRegister.cs:1696 / 2290 / 2919
        ↓  三处预处理，与硬件无关，两层共用
        1) addr 0x07（mixer）   : 把被 mask 通道的使能位 OR 进去
        2) addr 0x08/0x09/0x0A  : 音量，做 fadeout 减法 + 被 mask 就写 0
        3) YM2608 另有 0x11 tempo / 0x0B ADPCM 音量的 fadeout
        ↓
        RealModel 分支：
        scAY8910[chip].SetRegister(addr + 0x000, data)   ChipRegister.cs:1732
        scYM2203[chip].SetRegister(addr, data)           ChipRegister.cs:2329
        scYM2608[chip].SetRegister(port*0x100 + addr, data) ChipRegister.cs:3121
        ↓
RScciSoundChip.SetRegister → realChip.setRegister(adr,dat)   RealChip.cs:846
```

**结论 1：SSG 部分没有任何特殊分支。** 三个音量寄存器走的是和 FM 完全相同的
函数、相同的转发、相同的编码。没有单独的 SSG PCM 路径，也没有任何抽取。

`YM2149` 更是连独立入口都没有，只在 `ChipRegister.cs:6002` 出现：

```csharp
//YM2149が選ばれている場合は周波数を2倍にする
if (ct == Nc86ctl.ChipType.CHIP_YM2149)
```

即 YM2149 被当成 AY8910/ADPCM 的兼容芯片处理。

## 4. 节流全部在调用方，而且粒度很粗

`Driver/vgm.cs:276-290`，在 VGM 命令循环里：

```csharp
countNum++;
if (countNum > 100) {
    if (model == EnmModel.RealModel && countNum % 100 == 0) {
        isDataBlock = true;
        chipRegister.sendDataYM2608(0, model);
        chipRegister.setYM2608SyncWait(0, 1);
        chipRegister.sendDataYM2151(0, model);
        chipRegister.setYM2151SyncWait(0, 1);
        chipRegister.sendDataYM2608(1, model);
        chipRegister.setYM2608SyncWait(1, 1);
        chipRegister.sendDataYM2151(1, model);
        chipRegister.setYM2151SyncWait(1, 1);
    }
}
```

`ChipRegister.cs:5242`：

```csharp
public void sendDataYM2608(byte chipID, EnmModel model) {
    if (model == EnmModel.VirtualModel) return;
    if (scYM2608[chipID] != null && ctYM2608[chipID].realChipInfo[0].UseWait) {
        realChip.SendData();                          // 批量推出
        while (!scYM2608[chipID].IsBufferEmpty()) { }  // 等排空
    }
}
```

`ChipRegister.cs:5208`：

```csharp
public void setYM2608SyncWait(byte chipID, int wait) {
    if (scYM2608[chipID] != null && ...realChipInfo[0].UseWait)
        scYM2608[chipID].SetRegister(-1, (int)(wait * (UseWaitBoost ? 2.0 : 1.0)));
}
```

地址 `-1` 是带外命令（不是芯片寄存器），唯一的"显式插入等待"手段。

**结论 2：每帧的 SyncWait 被注释掉了。** `Driver/vgm.cs:303-310`：

```csharp
if (model == EnmModel.RealModel) {
    if (vgmSpeed == 1) {   // 等速の場合のみウェイトをかける
        if (useChipYM2612Ch6)
            chipRegister.setYM2612SyncWait(0, vgmWait);
        //if ((useChip & enmUseChip.SN76489) == enmUseChip.SN76489)
        //    chipRegister.setSN76489SyncWait(vgmWait);
        //chipRegister.setYM2608SyncWait(vgmWait);
        //chipRegister.setYM2151SyncWait(vgmWait);
    }
}
```

只有 YM2612 Ch6 保留了每帧的 `SyncWait`，SSG 的两行都被注释。
所以 SSG 的节流只剩"每 100 条命令一次 SendData + 全量排空"。

## 5. YM2612 DAC（Ch6 PCM）：这里才能看到真正的"降采样"

SSG 那条链没有丢弃，但 YM2612 的 6 通道 DAC PCM 有，而且丢得很明确。

### 5.1 VGM 侧：DAC 流由 `dacControl` 定时重新生成

`Driver/vgm.cs:1295` `VcWaitNSamplesAndSendYM26120x2a()` 读一个字节直接写 `0x2A`；
而带 stream control 的 VGM 走另一条路：

- `VcSetupStreamControl()` → `dacControl.setup_chip(...)`：记录目的芯片/命令
- `VcSetStreamData()` → `dacControl.set_data(...)`
- `VcSetStreamFrequency()` → `dacControl.set_frequency(...)`
- `VcStartStream()` → `dacControl.start(...)`

每个音频帧调一次 `OneFrameVGMStream()`（`Driver/vgm.cs:322`）：

```csharp
for (int CurChip = 0x00; CurChip < DacCtrlUsed; CurChip++)
    dacControl.update(DacCtrlUsg[CurChip], 1);
```

### 5.2 `dacControl.update()` 就是重采样器

`dacControl.cs:186`，关键三行：

```csharp
private const int DAC_SMPL_RATE = 44100;   // "DAC control 独自のサンプルレート (Fixed)"

chip.Step += samples;
NewPos = muldiv64round(chip.Step * chip.DataStep, chip.Frequency, DAC_SMPL_RATE);

while (chip.RemainCmds != 0 && chip.Pos < NewPos) {
    sendCommand(chip);              // 真正写芯片的那一下
    chip.Pos += chip.DataStep;
    chip.RemainCmds--;
}
```

读法：**固定的 44100 Hz 时基**（`DAC_SMPL_RATE`，注释写明是 dacControl 自己定的、
与输出采样率无关）× 原始流频率 → 算出这一帧应该推进到哪个样本位置，
然后只把 `Pos < NewPos` 范围内的命令发出去。

`sendCommand()` 有双重门（`dacControl.cs:24`）：

```csharp
if ((chip.Running & 0x10) != 0) return;   // 命令已经发过
if (chip.DataStart + chip.RealPos >= chip.DataLen) return;   // 数据到头
```

**结论 3：`chip.Pos` 一旦落后于时钟就不再追赶，超出 `DataLen` 的数据直接不发。**
这正是显式的降采样 / 丢点，也是真实芯片播放 YM2612 PCM 音质劣于仿真器的原因。
另外还有一个"快进 hack"（`dacControl.cs:206`）：`samples > 0x20` 时
`NewPos = chip.Step + (samples - 0x10)`，跳过中间的样本只保留最后一小段。

### 5.3 真实芯片上 0x2A 还会被整体丢弃

`ChipRegister.cs:4466`，`OnlyPCMEmulation` 打开时：

```csharp
if (ctYM2612[chipID].realChipInfo[0].OnlyPCMEmulation) {
    if (dPort == 0 && dAddr == 0x2b)
        scYM2612[chipID].SetRegister(dPort * 0x100 + dAddr, dData);   // DAC enable 照发
    else if (dPort == 0 && dAddr == 0x2a)
    {
        // 空块 —— 0x2A 采样数据不发给真实芯片，全交给仿真器发声
    }
    else
        scYM2612[chipID].SetRegister(...);
}
```

也就是说 MDPlayer 给 6 通道 PCM 准备了两条退路：

1. `OnlyPCMEmulation`（界面上的"PCM 只用仿真"）：PCM 完全不出 USB，由软件合成，
   真实芯片只负责 FM 通道；
2. 不开这个选项时：DAC 流经 `dacControl` 以 44100 Hz 固定时基重采样后再送。

### 5.4 三种 PCM 的对待方式对比

| 芯片/通道 | 真实芯片写入 | 节流手段 | 结果 |
|---|---|---|---|
| AY8910 / YM2203 / YM2608 SSG | 全部转发 | 每 100 条命令一次 `SendData` + 排空 | 无显式丢点，靠往返周期间接限制 |
| YM2612 Ch6 DAC | `dacControl` 重采样后转发；或整体不发 | **固定 44100 Hz 时基重采样 + 超界丢弃**，或 `OnlyPCMEmulation` 直接卸载到软件 | 显式降采样 |
| YM2612 其它寄存器 | 全部转发 | 每帧 `setYM2612SyncWait(0, vgmWait)` | 有显式等待 |

`useChipYM2612Ch6`（`Driver/vgm.cs:111,178`）只有在曲目用到 Ch6 时才置位，
它同时决定"每帧是否插 SyncWait"和"是否启用 dacControl 重采样"。

## 6. 对音质的解释

两种 PCM 在真实芯片上音质下降的原因不同，要分开看。

**SSG PCM（AY/YM2203/YM2608 通道）**：`SendData()` + `while(!IsBufferEmpty())`
是一次完整往返，毫秒级；而 SSG PCM 需要亚毫秒级更新音量寄存器。在"100 条一批 +
每批等排空"的结构下，排空期间新产生的写入只能堆在队列里，队列满就是 DLL / 硬件
侧丢弃，有效更新率被往返周期卡住——等价于降采样。
注意 SSG 这条路上 **MDPlayer 的 C# 代码没有显式丢点**（搜 `間引` / `skip` /
`thinning` / `resampl`，一处都没有），劣化来自流控粒度。

**YM2612 Ch6 DAC PCM**：这里是**显式**的降采样，见 §5.2：
固定 44100 Hz 时基 + `while (Pos < NewPos)` 只发走在前面的样本，
再加上 `samples > 0x20` 时的快进 hack，以及 `OnlyPCMEmulation` 时 `0x2A`
根本不发。所以 Ch6 PCM 的音质必然低于仿真器，是设计使然而不是意外。

### 6.1 SSG PCM 和 SID digi 是同一类东西：寄存器写型 PCM

把两条路放在一起看，就能看出它们劣化的其实是同一个机制。

SSG PCM 和 SID `$D418` digi 都属于**寄存器写型 PCM**：音量寄存器本身就是 DAC，
**采样率 = CPU 写这个寄存器的频率**。这类 PCM 没有可寻址的样本缓冲，每一个采样点
都必须在链路上单独占一条命令；芯片侧没有"先攒后放"的余地。于是采样率上限等于
链路能承受的命令率，跟芯片本身无关。

按本项目实测的链路参数（一条命令 4 字节、150000 B/s 线速、每次 flush 2 ms latency）
把各种结构的可持续命令率列出来：

| 结构 | 每批 | 可持续命令率 | 等效 PCM 采样率上限 |
|---|---|---|---|
| 每条都 flush | 4 B | 493/s | 0.5 kHz |
| MDPlayer SSG：100 条一批 + 全量排空 | ≈400 B | ≈21 400/s | ≈21 kHz |
| sidplay：每视频帧一次 flush（实测最密 172 条） | 688 B | ≈26 100/s | ≈26 kHz |
| YM2608 ADPCM V0：256 条一批 | 1024 B | 29 000/s | 29 kHz |
| YM2608 ADPCM V1：4096 B 切片 | 4096 B | 34 900/s | 34.9 kHz |
| 线速本身（不付 latency） | — | **37 500/s** | **37.5 kHz** |

算式统一是 `条数 / (条数 × 4 / 150000 + 0.002)`。

两点口径说明：

- **MDPlayer 那一行是投影，不是实测**。MDPlayer 走的是 SCCI，一条命令的实际字节数
  由 SCCI 层决定，本项目没有去反汇编那个 DLL（按约定），所以这里只是把"100 条一批 +
  每批全量排空"这个**结构**套到本项目的 4 字节命令 + 2 ms latency 参数上。
  结论对结构有效，绝对值只能当量级。
- "可持续命令率"指**一帧接一帧连打、不留空**的极限。SID digi 实际是突发式
  （帧内密集、帧间稀疏），所以真实峰值受帧预算约束，见 §7.2。

读法一句话：**任何每周期写一次寄存器的 PCM，采样率都过不了 37.5 kHz，
实际装备下是 21–34 kHz。** 44.1 kHz 的 SSG digi 或 SID digi 在这套链路上
不是"音质差一点"，而是**必然要丢点**。

所以 MDPlayer 的两条 PCM 退路其实是同一件事的两种做法：

- YM2612 Ch6：**显式降采样**——自己定 44100 Hz 时基（§5.2），只发走到位置之前的
  样本，超界的命令直接丢弃；再开 `OnlyPCMEmulation` 就干脆一个字节都不发（§5.3）。
- SSG：**结构性降采样**——代码里搜不到任何丢点逻辑（§6），但"100 条一批 + 每批
  阻塞等排空"让有效更新率被往返周期钉住，效果等价于低通到 20 kHz 上下。

§7 要做的就是把同一套算术套到 RSID digi 上，回答"这两首到底需不需要降速"。

## 7. 与 SID digi 的对应关系，以及能给 SPFM 的启示

| MDPlayer | 我们这边 |
|---|---|
| `SetRegister` 只转发，不缓冲 | `sid_write()` → `spfm_write_path_a()` 只进缓冲 |
| `SendData()` 批量推出 | `spfm_flush()`（`deferred` 模式下每帧一次） |
| `IsBufferEmpty()` 等排空 | `spfm_flush_drain()` → `FT_GetStatus` 查发送队列（`--limit` 时启用） |
| 每 100 条命令一次排空 | 目前按 deadline 排空，不按条数 |
| `SetRegister(-1, wait)` 插等待 | `spfm_nop()`（固件 NOP tick），但播放路径没用 |
| `dacControl` 44100 Hz 固定时基重采样 + 超界丢弃 | `sid_set_volume_limit()`：`$D418` 固定时基 + 同格覆盖 + 重复值丢弃 |

SID 的 `$D418` digi 在结构上更接近 **SSG PCM**（都是"高频写一个音量寄存器"，
每周期写一次，采样率 = 写入率），而不是 YM2612 的 `0x2A` DAC 流：

- SSG 的音量寄存器在 mixer 关掉 tone/noise 后就是一个 4 bit DAC，
  曲子通过 CPU 直接写它；MDPlayer 对此**不做重采样**，只靠 USB 往返节流。
- YM2612 的 DAC 流是 VGM 的 stream 格式，音质由 player 决定怎么采样，
  所以 MDPlayer 才必须自己定 44100 Hz 时基并丢点。

### 7.1 先把口径钉死：这个协议传的是"几十 kB/s"，不是"几百 kB/s"

链路参数是 1.5 Mbaud + 8N1 = 10 bit/字节 → 150000 B/s。但这个 150 kB/s
**是线上字节**，包含每条命令 4 字节里 2 字节的协议头（`slot`、`port<<1`）。
真正被芯片消化掉的有效载荷只有一个字节：

| 口径 | 数值 | 说明 |
|---|---|---|
| 线上字节 | 150 kB/s | FT232 引脚上实打实发出去的，**协议开销也算** |
| 有效载荷 | **37.5 kB/s** | 每条命令 4 字节里只有 1 字节是 data，所以正好是 ÷4 |
| 扣除 latency timer | 29–34 kB/s | 每批要白付 2 ms（§8.1） |
| 寄存器写次数 | 37500 次/s | 与"有效载荷字节/秒"同值 |

所以"SPFM 只能传几十 kB/s、绝没有几百 kB/s"这个判断是对的，而且原因不是线速低，
而是**协议本身 4:1 的开销**——一个数据字节要花掉 4 个线上字节。就算把波特率翻倍，
有效载荷也只有 75 kB/s，仍然是"几十 kB 到接近一百"的量级。

按 §6.1 的算术，这意味着**任何"每个采样点写一次寄存器"的流式音频，采样率上限就是
29–37.5 kHz**，而且这还是把整条链路 100% 让给音频、不留给旋律/鼓声/其它声部的情形。
所有 44.1 kHz 的流式音频在这套硬件上都必须先降采样，这不是音质取舍而是硬约束。

### 7.2 RSID digi 实测：Giana Sisters 与 Turbo Outrun

`--rsid` 模型下跑满 60 秒，`sidextract` 统计的寄存器写次数
（曲子是确定性的，次数可复现；硬件侧排空行为另看播放时的 `tx N B starved M`）：

| | Giana Sisters | Turbo Outrun |
|---|---|---|
| 寄存器写总数 | 410 013 = **6835 次/s** | 380 899 = **6350 次/s** |
| 最忙的一秒 | 7434 次 = 20% 链路 | 8182 次 = 22% 链路 |
| 最忙的一帧（19.95 ms） | 176 次 = 4.69 ms 线时 | 192 次 = 5.12 ms |
| 加 flush 的 2 ms 后占帧预算 | 6.69 ms = **34%** | 7.12 ms = **36%** |
| 平均线时占用 | 18% | 17% |

**结论 4：这两首按原速率不会挤爆 USB，还有 4.6 倍余量。**
"RSID 写入太密所以必须限速"这个说法在 60 秒实测下不成立——最极端的一秒只用掉
五分之一带宽，最极端的一帧只用掉三分之一帧预算。

但**冗余很严重**，这是降采样的真正理由。把 `$D418`（音量 DAC）单独拎出来：

| | Giana | Turbo Outrun |
|---|---|---|
| `$D418` 写入 | 6335 次/s | 4595 次/s |
| 其中真正改了值 | 2133 次/s | 3173 次/s |
| 重复写同一个值 | **66%** | **31%** |
| 写入间隔中位数 | 0.147 ms（6.8 kHz） | 0.164 ms（6.1 kHz） |
| 值变化间隔中位数 | 0.159 ms（6.3 kHz） | 0.193 ms（5.2 kHz） |

Giana 三分之二的 `$D418` 写是在重复上一个值，芯片输出毫无变化。
**这部分不丢任何信息就能删掉**，和"降采样"是两件事。

### 7.3 `--limit` 保住多少信息：逐档实测

用 `--limit` 对同一份 20 秒渲染做离线抽取（规则与 `on_sid_write()` 一致），
保留量对照"真实值变化次数"：

| `--limit` | Giana 保留的值变化 | Outrun 保留的值变化 | Giana 总命令 | Outrun 总命令 |
|---|---|---|---|---|
| 不限 | 100%（32867） | 100%（67011） | 117 543 | 134 268 |
| **8000 Hz** | **100%** | **99.996%** | 41 794（36%） | 101 609（76%） |
| 6000 Hz | 91% | 95% | 38 804 | 98 412 |
| 5000 Hz | 80% | 88% | 35 293 | 93 716 |
| 4000 Hz | 69% | 79% | 31 580 | 87 345 |
| 3000 Hz | 56% | 65% | 27 474 | 78 444 |
| 2000 Hz | 40% | 47% | 22 163 | 66 352 |
| 1000 Hz | 23% | 25% | 16 390 | 51 653 |

两个读数：

1. **`--limit 8000` 对这两首几乎无损**（Giana 100%、Outrun 99.996%），却把
   Giana 的命令数砍到 36%。原因是它只丢"同一时基格子里的重复写"，而值变化之间的
   最小间隔是 125–150 µs，8 kHz（格子 125 µs）刚好能把每次变化装进去。
2. **真正的降采样分界线在 7 kHz 附近**。曲子自身改变 DAC 值的速度只有 1.6–3.2 k/s，
   但变化的瞬时密度很高；时基格子一旦宽过 150 µs，同类变化就开始合并。
   掉到 2 kHz 只剩四成变化，鼓声会明显发闷——那才是真的丢信息。

### 7.4 和 MDPlayer 的 SSG/PCM 退路对照，得出更明确的结论

把 §6.1 的表补上 SID 这一列，同一套算术算出同一个结论：

| 音频源 | MDPlayer 的处理 | 我们这边 | 有效载荷上限 | 是否必须降采样 |
|---|---|---|---|---|
| YM2612 Ch6 PCM | `dacControl` 44100 Hz 时基重采样，超界丢弃 | — | 34.9 kB/s | **必须**（44.1 kHz 塞不进去） |
| SSG PCM（AY/YM2203/YM2608） | 不重采样，靠"100 条一批 + 等排空"结构性限速 | — | ≈21 kHz 等效 | **事实上必须** |
| SID `$D418` digi（Giana / Outrun） | — | `--limit` 固定时基抽取 + 链路排空 | 37.5 kB/s | **不必须**（峰值仅 34% 帧预算） |

**明确的结论，一条一条：**

1. **协议上限就是几十 kB/s**（37.5 kB/s 有效载荷 / 150 kB/s 线上），因为 4 字节
   命令只带 1 字节数据。任何"几百 kB/s"的说法都不成立。
2. **凡是 44.1 kHz 的流式音频，在这套硬件上必然要降速** —— 每采样点一条命令的结构下，
   44.1 kHz 需要 44100 条/s × 4 = 176 kB/s 线上字节，超上限 18%，
   而且还要和其它声部抢带宽。这是 §6.1 那条表已经写死的硬约束。
3. **但 RSID digi 这两首不属于"必须降采样"那一类**。它们的峰值是 8182 次/s =
   33 kB/s 线上字节，占上限 22%，加上每帧 2 ms latency 也只占 34% 帧预算。
   它们的问题是**冗余**（Giana 有 66% 的写是重复值），不是**带宽**。
4. 所以正确的做法不是"给 RSID 限速才能出声"，而是：
   - 默认保留原速率（听得最准，链路有余量）；
   - 用 `--limit 8000` 免费删掉重复写（Giana 命令数 −64%，值变化 100% 保留）；
   - 只有在同时跑多首 / 多芯片争抢同一条 USB 链路，或把曲子拉到 44.1 kHz 目标采样率时，
     才需要压到 4 kHz 以下，那时掉的是真实信息，属于音质取舍。

**"绝大部分流式音频都要降速"这句话要分层读**：以 44.1 kHz 为目标的流式 PCM
（VGM 的 DAC 流、SSG digi、以及任何想按固定采样率重建波形的方案）确实绝大部分都要降；
而 C64 原生的 `$D418` digi 本身就运行在 5–7 kHz 的写入率上，它已经"降"过了——
瓶子（曲子）本身就没装满，不需要再倒掉。

### 7.5 落地状态

上面第 2、3 两条（链路侧排空 + 生成侧固定时基抽取）已经落地为 `--limit`
（见 `docs/驱动说明.md` §6.10）：生成侧 `on_sid_write()` 按固定时基抽取 `$D418`，
链路侧 `spfm_flush_drain()` 每批之后问一次 FTDI 发送队列。
"一是 digi 帧内的写入全部保留、攒成一批" 这条没有照做——用户要求的是"限速"，
不是"先攒后丢"，整批保留会让延迟以排空时间为步长增长。

上限速度的实测参照不在 MDPlayer 里，而在同族的 `Denjhang_Music_Player_v16`
（YM2608 ADPCM 预载），见 §8。

## 8. YM2608 ADPCM 预载：这套硬件上唯一上机验证过的全速写入

MDPlayer 的节流在 §4 已经说清（粗细粒度、SSG 那两行 SyncWait 被注释）。
但同族的 `Denjhang_Music_Player_v16` 里有一条路径是**零节流、整块灌数据、
并且真的在这套 SPFM 上跑通过**的：YM2608 ADPCM RAM 预载。它比 MDPlayer 更适合
当"上限速度"的参照，因为它把整条链路推向饱和而不做任何让步。

代码在 `src/windows/ym2608/ym2608_window.cpp:426-561`，一共**两种模式**：

| 模式 | 一次传多少 | 代码位置 | 结构 |
|---|---|---|---|
| **V0** chunk-per-flush | **256 条命令 = 1024 字节**，然后 `spfm_flush()` | `Ym2608AdpcmPreloadV0` | 循环里 reshape 到 `spfm_reg_t` 数组，逐批推 |
| **V1** raw buffer | **全部一次**，由 `spfm_write_raw` 按 **4096 字节**切片 | `Ym2608AdpcmPreloadV1` | 先拼完整命令缓冲（11 条 init + N 条数据 + 2 条 finalize），最后一次 flush |

`spfm_write_raw` → `spfm_lite.c:22` 的 `spfm_flush()` 恒定按 4096 字节切片
（`if (chunk > 4096) chunk = 4096`），所以 V1 的实际 USB 传输粒度就是 4 KiB。

node-spfm 的等价实现是 `src/player/ym2608-ram-write.ts`，用 256 条一批，
即 V0 形状。

**每条命令都是 4 字节**（`[slot, (port&7)<<1, addr, data]`），V0/V1 相同。
一次 2976 字节的 ADPCM 块 → 2989 条命令 → 11956 字节。

### 8.1 线速与批大小的换算

1.5 Mbaud 配 8N1 是 **10 bit/字节**（1 起始 + 8 数据 + 1 停止），
所以线速是 **150000 B/s = 6.667 µs/字节**：

| 记录 | 字节 | 线上时间 | 线速上限 |
|---|---|---|---|
| Path A / SPFM Light 命令 | 4 | 26.667 µs | **37500 条/s** |
| Path B | 3 | 20.0 µs | 50000 条/s |

（文档里流传的"约 4 万次/s"介于这两者之间，是 Path B 加上固件开销的实测值。）

但真正卡住吞吐的是**每次 flush 后的 2 ms latency timer**：数据推完，线路空转
2 ms 才开始下一批。可持续速率 = 批字节数 / (线上时间 + 2 ms)：

| 每批 | 线上时间 | +2 ms | 折算 | 相当于 |
|---|---|---|---|---|
| 4 B（1 条） | 0.03 ms | 2.03 ms | 493 条/s | 每条都 flush |
| 1024 B（V0，256 条） | 6.83 ms | 8.83 ms | **29000 条/s** | 78% 线速 |
| 4096 B（V1 切片） | 27.31 ms | 29.31 ms | **34900 条/s** | 93% 线速 |
| 65536 B（缓冲满） | 436.9 ms | 438.9 ms | 37300 条/s | 99.5% 线速 |

所以 V1 是能贴着天花板跑的形状，V0 为往返付出约 23% 的代价——这也解释了
为什么参考项目把 V1 标成 "node-spfm compatible"：它才是真的全速。

### 8.1.1 换算成 kB/s：这就是"最大速度"的数字

ADPCM 预载的每条命令都是 `[slot, port<<1, 0x08, data]`，**一条命令带一个数据字节**，
所以"命令条/秒"和"数据字节/秒"是同一个数，"线上字节/秒"是它的 4 倍。三个口径分别是：

| 口径 | 含义 | 上限 |
|---|---|---|
| 线上字节 | FT232 实际发出去的字节（含协议开销） | **150 kB/s**（= 1.5 Mbaud ÷ 10 bit × 1000） |
| 数据字节 | 真正落进芯片 RAM 的 ADPCM 样本 | **37.5 kB/s**（= 线上 ÷ 4） |
| 命令条数 | 寄存器写次数 | **37500 条/s**（= 数据字节数） |

这是**硬上限**，任何模式都不可能超过：一个数据字节在 Path A 上必须花 4 个线上字节，
没有打包/压缩的余地。V1 是唯一能逼近它的形状，因为它把整块拼成一个缓冲、只 flush
一次，2 ms 延迟计时器只付一次：

| 模式 / 一批的数据量 N | 线上字节 | flush 次数 | 耗时 | 数据速率 | 线上速率 | 占上限 |
|---|---|---|---|---|---|---|
| V0：N = 256（显式 flush） | 1 024 | 1 | 8.83 ms | **29.0 kB/s** | 116 kB/s | 77% |
| V1：N = 256 | 1 076 | 1 | 9.17 ms | 27.9 kB/s | 117 kB/s | 74% |
| V1：N = 4 096 | 16 436 | 1 | 111.6 ms | 36.7 kB/s | 147 kB/s | 98% |
| V1：N = 16 384 | 65 588 | 2 | 441.3 ms | 37.1 kB/s | 149 kB/s | 99% |
| V1：N = 65 536 | 262 196 | 5 | 1 758 ms | 37.3 kB/s | 149 kB/s | 99% |
| V1：N → ∞ | — | — | — | **37.5 kB/s** | 150 kB/s | 100% |

耗时 = 线上字节 ÷ 150000 + flush 次数 × 0.002。flush 次数不是 1 的原因在
`spfm_lite.c:97`：`spfm_write_raw` 一旦放不下就先 flush，而缓冲区是 64 KiB，所以
V1 每 65536 字节就会自己断一次，每次仍然付 2 ms。V0 是每 1024 字节显式
`spfm_flush()`，所以付得最狠。

N = 256 时 V1 并不比 V0 快，因为 2 ms 摊到 256 个字节上仍然是 22% 的开销；
**要把延迟计时器摊薄，只能靠"一批带更多数据"，靠线速参数调不动它。**

所以以后说"这套 SPFM 的最大写入速度"，口径写全：

> **数据 37.5 kB/s / 线上 150 kB/s（V1 渐近，理论上限）；V0 形状实测 29.0 / 116 kB/s。**
> 折成 SID 寄存器写：**37500 次/s** 上限，V0 形状 **29000 次/s**。

`sidtest bench` 里的 `V1 4096B 切片`、`V0 256 条/批` 两行就是上表第 1 行和第 3 行，
实测值除以理论值给的就是"固件开销吃掉多少"。

### 8.2 在本机复现这套测量

`sidtest bench` 直接照搬这两种形状（写 `0x04 <- 0x00`，gate 关所以静音；
固件不做去重，总线工作量与真实写入相同），并额外跑两条本项目实际用到的形状：

```
  V1 全量单批（4096B 切片）
  4096B/批 = 1024 条
  V0 256 条/批（1024B）
  V0 形状 + 每批等排空
  一帧 172 条（sidplay 不限速；Giana 平均 5434 条/s，最密一帧 172）
  一帧 34 条 （sidplay --limit 2000）
  每条 flush （最坏情况）
```

每行都给出实测值、含 2 ms latency 的理论值和二者比值。比值低于 80% 说明链路上
还有固件侧开销（STM32 每条命令都要解析 + 驱动总线），这是理论模型里没有的项，
也正是"不能只信理论值"的原因。

### 8.3 对 SID digi 的意义

`sidplay` 是**每视频帧一次 flush**（19.96 ms），比 V0 的 1024 B/批划算得多。
Giana 20 秒里最密的一帧是 **172 条 = 688 字节**，线上 4.59 ms + 2 ms = 6.59 ms，
只占一帧预算的 33%（60 秒窗口下最密 176 条、34%）。**即使不限速，整条流也装得下**。
逐首实测和 `--limit` 的保留率见 §7.2–§7.3。

结论修正：`--limit` 不是"救命"开关，而是把更新率从曲子的密度里拿出来交给自己
定的工具。链路侧排空检查（`tx N B starved M`）的作用是把"跟不上"变成可见数字。

## 9. 数据来源

本仓库文件：
- `MDPlayer/MDPlayerx64/RealChip.cs`（211, 229, 758, 805, 846, 855, 861）
- `MDPlayer/MDPlayerx64/ChipRegister.cs`（1696, 1732, 2290, 2329, 2919, 3121,
  4277, 4383, 4466, 5208, 5216, 5242, 5362）
- `MDPlayer/MDPlayerx64/Driver/vgm.cs`（111, 178, 276-290, 303-310, 322,
  589, 664, 676, 682, 1295, 1320-1425）
- `MDPlayer/MDPlayerx64/dacControl.cs`（11, 24, 186, 400, 467）

实际读的是 `E:\working\vscode-projects\MDPlayer-STBL449\MDPlayer-STBL449\MDPlayer\MDPlayerx64\`
下的同名文件；`dacControl.cs` 的 `update()` 在 186-251 行，
`DAC_SMPL_RATE` 常量与 `muldiv64round` 在 180-184 行，`samples > 0x20` 的
快进分支在 206-217 行。

仿真器（MDSound）源码未列入，本文不对其做任何结论。

`--rsid` 实测数据（§7.2–§7.3）由本仓库的 `sidextract --rsid --seconds 60`
（及 `--limit`）在本机生成，输入是 `sid/Great_Giana_Sisters-rsid.sid` 与
`sid/turboutrun.sid`。写次数来自仿真器对 6502 的逐指令执行统计，
可复现；硬件侧的排空行为要看 `sidplay` 打印的 `tx N B starved M`。

`Denjhang_Music_Player_v16` 文件（§8）：
- `src/windows/ym2608/ym2608_window.cpp`（426-485 V0，489-561 V1，565-570 分发）
- `src/windows/sn76489/spfm_lite.c`（12 缓冲区，22 4096 切片，42/97 写入口）
- `src/windows/spfm/spfm_manager.cpp`（95-101 1.5 Mbaud / 8N1 / 2 ms latency）
- `src/core/vgm_sync.cpp`（1267-1284 0x81 数据块分发，1465-1478 阻塞命令折算）

`node-spfm-denjhang-main/src/player/ym2608-ram-write.ts`：256 条/批的等价实现。
