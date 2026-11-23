# libsidplayfp 构建（本项目唯一的音质标杆）

## 为什么是它

`sid\turboutrun.sid` 20 秒，同一个文件：

| 引擎 | digi 检测 | RMS | 峰值 | 鼓点瞬态（10 ms 窗，>3×） |
|---|---|---|---|---|
| **libsidplayfp 3.1.2 + reSIDfp 1.3.0** | — | 1004 | 14352 | **55** |
| WebSid（原版 `computeAudioSamples`） | `type=0 NONE` | 2043 | 28925 | **0** |

WebSid 把数字鼓整段丢了，所以 **WebSid 不严肃，不能当标准**。听感 A/B：
`build\lsfp_to.wav`（有鼓）vs `build\ws_to.wav`（无鼓）。

## 装了什么、装在哪

```
third_party/upstream/libsidplayfp-3.1.2/     # release tarball 解压，原样
third_party/upstream/libresidfp-1.3.0/       # reSIDfp 是独立项目，原样
third_party/local/lib/libsidplayfp.a         # 静态库
third_party/local/lib/libresidfp.a
third_party/local/include/sidplayfp/...      # 头文件
```

## 三个坑

1. **必须用 release tarball，不能用 git archive / master.zip**
   `psiddrv.bin`、`sidplayer1.bin`、`sidplayer2.bin` 是 `xa65` + `od` 从 `.a65`
   生成的，仓库里没有，只有 release 包里带。缺了会报 `PSID_DRIVER` 未定义。
2. **`configure` 必须在 msys64 的 bash 里跑，不能用 Git Bash**
   Git Bash 的 autoconf 看不到 `gcc`，直接 `no acceptable C compiler found`：

   ```bash
   /d/msys64/usr/bin/bash.exe -lc '
     export PATH=/mingw64/bin:/usr/bin:/bin
     cd .../libresidfp-1.3.0
     ./configure --prefix=.../third_party/local --disable-shared --enable-static
     make -j4 && make install
   '
   ```

   libsidplayfp 同样，但要多给 `PKG_CONFIG_PATH=.../third_party/local/lib/pkgconfig`，
   configure 会打印 `checking for libresidfp >= 1.0.0... yes`。
3. **最后一步 `libtool: error: Could not determine host file name ... Continuing` 是无害的**
   `src/.libs/libsidplayfp.a` 已经正确生成，`make install` 也正常。

## 参考渲染器

`tools/lsfpref.cpp`（`make ref` 编出 `build/lsfpref.exe`）是**最小参考实现**，只调官方
API，不含任何本项目自己写的 SID 逻辑：

```cpp
sidplayfp engine;
ReSIDfpBuilder rs("residfp");          // 不要调 rs.create()，它是 protected
SidConfig cfg;
cfg.frequency = 44100;
cfg.samplingMethod = SidConfig::INTERPOLATE;
cfg.sidEmulation = &rs;
engine.config(cfg);
engine.load(tune);
engine.initMixer(true);                // 立体声
loop: int res = engine.play(5000); engine.mix(buf, res);   // res 是交错采样数
```

用法：

```powershell
# 标杆 WAV
.\build\lsfpref.exe sid\turboutrun.sid to_ref.wav 20

# 指定子曲 / 强制 8580
.\build\lsfpref.exe sid\Golden_Axe.sid ga.wav 30 3
.\build\lsfpref.exe sid\Golden_Axe.sid ga8580.wav 30 3 8580
```

重编静态库后要重新链 `lsfpref.exe`（也可以直接 `make ref`）：

```powershell
$P="third_party/local"
g++ -std=c++17 -O2 -I$P/include tools/lsfpref.cpp `
    $P/lib/libsidplayfp.a $P/lib/libresidfp.a -static -o build/lsfpref.exe
```

## 判据

- 曲目有鼓/digi 时，先出 `lsfpref.exe` 的 WAV 作为参照；
- 我们自己的录制/渲染必须和它在**同一段**听到同样的鼓点数量与位置；
- 只有通过了这一步，才谈"精度"和"体积"。

## 现在已经接进三个程序

`src/sid_lsfp.cpp` 把这两个库包成 C 接口，`sidextract` / `sidvgmplay` /
`spfmplayer` 都用它：

- **录制**（`--engine lsfp`）：自定义 `sidbuilder` + `sidemu`，`sidemu::write()` 是
  所有 `$D400-$D41F` 写入的唯一入口，在那里记下 libsidplayfp 自己的 PHI1 时钟。
  捕获 SID 内部还挂一片真的 `reSIDfp::residfp`，因为曲子会读 `$D41B/$D41C`
  并据此分支；返回 0 会让 6502 走错路、录出另一条流。
- **音频**（`--model lsfp`）：直接驱 `reSIDfp::residfp` 的 `write/clock`，
  `DECIMATE` 对应 `SidConfig::INTERPOLATE`，就是 `reSIDfpEmu` 内部做的事。
- **直通**（`sidvgmplay <tune.sid>` 默认）：完全不录制，走
  `config/load/initMixer/play/mix`，与 `lsfpref.exe` 输出逐字节相同。

两个必须固定的点，否则同一文件两次结果不同：

| 设置 | 不固定会怎样 |
|---|---|
| `cfg.powerOnDelay = MAX_POWER_ON_DELAY` | 超过 MAX 会用时间种子随机化，读到自由运行计数器的曲子相位全变 |
| 每次都调 `tune.selectSong()`，默认传 0 | 不调用会留下不同的初始状态，Turbo Outrun 前 11ms 就变了 |

`mix()` 交出的采样数是 `play()` 那一段的全部；按调用方块大小截断会丢采样并
让整首错位，所以 `src/sid_lsfp.cpp` 里留了 pending 缓冲。

编译这两个文件需要的不是公开头文件，还要 libsidplayfp 源码树里的内部头
（`src/sidemu.h` 等）。`makefile` 里的 `LS_CXXFLAGS` 已经带上了。

## 流式录制：`spfmplayer.exe` 用的那条路

上面的 `sid_lsfp_extract()` 是"录 N 秒再交出来"，适合出文件，不适合听：一首
`.sid` 没有结尾，先录完就没有"一直在播"这回事了。

`sid_lsfp_stream_*()` 是同一套捕获 SID，但边界换成帧：`sid_lsfp_stream_next()`
只把引擎推到下一帧边界，返回这一帧的写入。第 N+1 帧的仿真发生在第 N 帧还在
芯片上响的 19.96 ms 里，所以没有停顿、也不需要预渲染。

时间戳是**从第一帧 play 突发算起的绝对 PHI1 周期**，帧边界钉在
`origin + k * frame_cycles`，分块大小不影响结果。

三个坑，都是实测踩出来的：

| 现象 | 原因 | 处理 |
|---|---|---|
| 整首被推迟 163 ms（R-Type） | init 写了 160 ms 才有第一个 play 突发，搜索只给了 3 帧就放弃 | 搜索窗口放到 1 秒（`clock_hz * 1.0`） |
| 某些曲目帧边界抖动 | 用 `run` 计数当边界，会随分块累积 | 边界用 `origin + k*frame_cycles`，`run` 只用来决定还要推多少 |
| 最后一帧写入丢掉 | `finished` 后直接返回 -1 | 带 halt 的那一帧仍然把写入交出去，再返回 -1 |

一致性用 `tools/streamcheck.cpp`（`make ref`）守住：

```bash
./build/streamcheck.exe sid/turboutrun.sid 10
# extract : 47337 writes, last 9999.95 ms
# stream  : 47340 writes over 546 frames, last 10000.43 ms
# compare first 47337 writes (up to 10000.43 ms): identical (0 mismatches)
```

它把两条路径的每一条写入（寄存器、值、时间戳 ±1 ms）逐条比对。目前
R-Type / Turbo Outrun / Giana RSID / Ambient / Alcorythm / Golden Axe /
Echoes / 3_Ankare / L.E.D. Storm 全部 0 mismatch。
