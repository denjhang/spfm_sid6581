# SPFM SID 6581 驱动

SID 6581 / 8580 挂到 SPFM 并行总线的驱动 + 控制台冒烟测试。

**硬件：74LS374 + 74HCT02 + 74HCT32 + 74HC74**（地址锁存 + 相位译码 + 写窗口整形），
已上机验证。SID 接在 **slot 3**（RE-SPFM 最后一个槽），PHI2 = **1.000 MHz**。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/写入时序调试记录.md](docs/写入时序调试记录.md) | **实测记录**：为什么只有 repeat 能稳定发声、SPFM 与 SID 的时序不匹配分析 |
| [docs/接线表_374_02_min.md](docs/接线表_374_02_min.md) | **✅ 已验证的接线表（现行）**：374 + 02 + 32 + 74HC74，四颗 IC |
| [docs/接线表_374_02_74.md](docs/接线表_374_02_74.md) | 方案索引与选型对比（含 164/595/174/273 四份备选） |
| [docs/接线表_374_02_123.md](docs/接线表_374_02_123.md) | 旧改进方案（74HC123 单稳态，**已废弃**） |
| [docs/接线表_374_02.md](docs/接线表_374_02.md) | 旧接线（写窗口过短，即问题来源） |
| [docs/交接文档.md](docs/交接文档.md) | 项目状态、寄存器速查、风险清单 |
| [docs/驱动说明.md](docs/驱动说明.md) | 驱动 API、写模式、字节流验证方法 |

## 当前状态

**写时序已解决，硬件方案已验证可用。**

问题曾是：只有 `--repeat` ≥ 4 才能稳定发声，且音高不跟随指令。根因是 `/CS` 与 `R/W`
两路不同源（一路过两个 NOR 门、一路裸接 `/WR`），SID 在同一 PHI2 沿采样时可能采到不一致的组合。

现在的做法（详见 [接线表_374_02_min.md](docs/接线表_374_02_min.md)）：

1. `TRIG_n = ¬A0 ∨ /WR ∨ CSn` 把写条件编码成一个信号；
2. `TRIG_n` 经 **74HC74** 整形缓冲成 `Wn`（实测跳过这一级单次写入会失真）；
3. `Wn` **同时**驱动 SID 的 `/CS`（pin6）和 `R/W`（pin7），同源零偏斜；
4. `PHI2` 由 1 MHz 晶振独立供给（pin8）。

四颗 IC：74LS374 + 74HCT02 + 74HCT32 + 74HC74。不需要移位寄存器，不需要单稳态。

## 编译

MinGW-w64（本机在 `D:\msys64`）：

```bash
export PATH="/d/msys64/mingw64/bin:/d/msys64/usr/bin:$PATH"
make
```

产物：`build/sidtest.exe`（已静态链接，只需同目录的 `ftd2xx64.dll`）。

## 用法

```bash
./build/sidtest.exe list                        # 枚举 FTDI 设备
./build/sidtest.exe init                        # 复位 + 初始化 slot 3
./build/sidtest.exe selftest                    # 不上硬件，验证生成的字节流
./build/sidtest.exe scale                       # C 大调音阶（方波）
./build/sidtest.exe scale --wave saw --filter   # 锯齿 + 低通滤波
./build/sidtest.exe chord                       # C / Am / F / G / C 三声部
./build/sidtest.exe music                       # 三声部小曲（默认小星星）
./build/sidtest.exe music --song ode            # 欢乐颂
./build/sidtest.exe music --song birthday       # 生日快乐
./build/sidtest.exe music --song bach           # 巴赫（移植自 Arduino_SID/SID_Bach）
./build/sidtest.exe music --list                # 列出所有曲子
./build/sidtest.exe music --bpm 160 --loops 3   # 改速度和重复次数
./build/sidtest.exe noise                       # 噪声频率扫描
./build/sidtest.exe pwm                         # 占空比扫描
./build/sidtest.exe filter                      # 滤波器截止频率扫描
./build/sidtest.exe adsr                        # 四组 ADSR 对比
./build/sidtest.exe checkup                     # 上电五项检查（需要万用表/示波器）
./build/sidtest.exe interactive                 # 手敲寄存器
./build/sidtest.exe --dry-run scale             # 不连硬件，打印解码后的总线动作
```

常用选项：`--slot N`（默认 3）、`--device N`（默认 0）、`--write-mode`、`--gap N`、`--verbose`。

## 关键约束

- 复位后 `0x18`（音量）= 0，**不写就没有声音**
- 初始化要先清零全部寄存器（374 上电后 Q 输出不定）
- `TRIG_n = ¬A0 ∨ /WR ∨ CSn` 必须经 74HCT32 译码；直连 CSn 会在地址相写入垃圾值导致起音咔哒
- `TRIG_n` 必须再经 74HC74 整形才送 SID；实测跳过这一级单次写入会明显失真
- SID 的 `/CS`（pin6）与 `R/W`（pin7）必须接同一个 `Wn`，两路分开会引入偏斜
- SID 的 pin8 是 PHI2，不是写信号；接错则完全无时钟
- 6581 需要 +12V（8580 是 +9V），缺了完全无声且很难察觉
- `0x18` 的 bit4–7 是滤波器模式 + 声部 3 静音，**不是波形开关**
