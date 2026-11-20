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

## 7. 与 SID digi 的对应关系，以及能给 SPFM 的启示

| MDPlayer | 我们这边 |
|---|---|
| `SetRegister` 只转发，不缓冲 | `sid_write()` → `spfm_write_path_a()` 只进缓冲 |
| `SendData()` 批量推出 | `spfm_flush()`（`deferred` 模式下每帧一次） |
| `IsBufferEmpty()` 等排空 | FTDI 发送缓冲 / 固件忙信号（**目前没有检查**） |
| 每 100 条命令一次排空 | **没有对应机制** |
| `SetRegister(-1, wait)` 插等待 | `spfm_nop()`（固件 NOP tick），但播放路径没用 |
| `dacControl` 44100 Hz 固定时基重采样 + 超界丢弃 | **没有对应机制** |

SID 的 `$D418` digi 在结构上更接近 **SSG PCM**（都是"高频写一个音量寄存器"），
而不是 YM2612 的 `0x2A` DAC 流：

- SSG 的音量寄存器在 mixer 关掉 tone/noise 后就是一个 4 bit DAC，
  曲子通过 CPU 直接写它；MDPlayer 对此**不做重采样**，只靠 USB 往返节流。
- YM2612 的 DAC 流是 VGM 的 stream 格式，音质由 player 决定怎么采样，
  所以 MDPlayer 才必须自己定 44100 Hz 时基并丢点。

因此对我们的直接启示：**采样率控制在生成侧，链路侧只做"按时批量 + 等排空"**。
具体三件事：

1. digi 帧内的 `$D418` 写入全部保留，攒成一批；
2. 每批之后确认链路排空，而不是盲目按时间推；
3. 用"一批排空耗时"反推一批最多能装多少条——超出的部分在生成侧就按固定时基
   （对应 MDPlayer 的 44100 Hz）抽取，而不是让 USB 队列随机丢。

带宽本身不是问题：1.5 Mbaud 8N1 = 187.5 kB/s，6785 条/秒 × 4 字节 = 27.1 kB/s，
只占 14%。瓶颈在每批的往返次数，不在数据量。

## 8. 数据来源

本仓库文件：
- `MDPlayer/MDPlayerx64/RealChip.cs`（211, 229, 758, 805, 846, 855, 861）
- `MDPlayer/MDPlayerx64/ChipRegister.cs`（1696, 1732, 2290, 2329, 2919, 3121,
  4277, 4383, 4466, 5208, 5216, 5242, 5362）
- `MDPlayer/MDPlayerx64/Driver/vgm.cs`（111, 178, 276-290, 303-310, 322,
  589, 664, 676, 682, 1295, 1320-1425）
- `MDPlayer/MDPlayerx64/dacControl.cs`（11, 24, 186, 400, 467）

仿真器（MDSound）源码未列入，本文不对其做任何结论。
