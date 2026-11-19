# SPFM SID 6581 驱动

SID 6581 / 8580 挂到 SPFM 并行总线的驱动 + 控制台冒烟测试。

**硬件已确定：74LS374 + 74HCT02**（地址锁存 + 相位译码）。SID 接在 **slot 3**（RE-SPFM 最后一个槽），
PHI2 = **1.000 MHz**。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/写入时序调试记录.md](docs/写入时序调试记录.md) | **实测记录**：为什么只有 repeat 能稳定发声、SPFM 与 SID 的时序不匹配分析 |
| [docs/接线表_374_02_74.md](docs/接线表_374_02_74.md) | **方案索引与选型对比**（不含接线细节） |
| ↳ [74_164](docs/接线表_374_02_74_164.md) / [74_595](docs/接线表_374_02_74_595.md) / [74_174](docs/接线表_374_02_74_174.md) / [74_273](docs/接线表_374_02_74_273.md) | **四份各自独立的完整接线表**，网络标签一致，选一份照焊 |
| [docs/接线表_374_02_123.md](docs/接线表_374_02_123.md) | 旧改进方案（74HC123 单稳态，**已废弃**） |
| [docs/接线表_374_02.md](docs/接线表_374_02.md) | 当前在用的接线（写窗口只有数百 ns，即问题所在） |
| [docs/交接文档.md](docs/交接文档.md) | 项目状态、寄存器速查、风险清单 |
| [docs/驱动说明.md](docs/驱动说明.md) | 驱动 API、写模式、字节流验证方法 |

## 当前状态（重要）

驱动和测试程序已完成，**但写时序尚未解决**：

- 只有 `--repeat` ≥ 4 能让 SID 稳定发出延音；
- 单次写入（repeat=1）是概率性的，每次运行能成功的时序参数都不同；
- repeat 换来稳定发声的代价是**音高不跟随指令**（固定错误音高）。

怀疑原因是 SID 要求写入必须跨过 PHI2 上升沿，而 SPFM 的 `/WR` 脉冲与模块上的
1 MHz 晶振没有任何相位关系。详见
[写入时序调试记录](docs/写入时序调试记录.md)。

当前默认值：`two-phase-nop`、`gap=3`、`repeat=12`。

## 快速理解

SPFM 单槽 16 根总线（D0–D7、A0–A3、CSn、/WR、/RD、IC）全是协议信号，没有 GPIO。

SID 需要 A0–A4 五根地址，总线只有四根，所以：

- **D0–D7 分时复用**：地址相送地址、数据相送数据；
- **A0 当相位位**：0 = 地址相，1 = 数据相；
- **74LS374** 在地址相把 A0–A4 锁住；
- **74HCT02** 译出 `LE` 和 `/CS_sid`，保证地址相不误写。

驱动层每次寄存器写就是一条 4 字节命令：

```
[slot, 0x00, addr & 0x1F, data]
```

固件自动展开成两次总线写。**驱动不需要操作任何控制线。**

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
- `/CS` 必须经 74HCT02 门控，直连会在地址相写入垃圾值导致每次起音咔哒
- 6581 需要 +12V（8580 是 +9V），缺了完全无声且很难察觉
- `0x18` 的 bit4–7 是滤波器模式 + 声部 3 静音，**不是波形开关**
