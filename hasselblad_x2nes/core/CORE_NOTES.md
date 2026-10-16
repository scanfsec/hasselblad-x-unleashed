# LaiNES 核心移植笔记（x2nes / 哈苏 X2D II 平台）

> 本文档由源码检视自动整理，记录 LaiNES 的版本、文件结构、核心与 SDL2 平台层的
> 全部接口耦合点、mapper 支持与编译注意事项。目标：在自有平台层（Android NDK，
> libc++_static）下复用其纯核心，**不改动 LaiNES 核心源码本身**。

## 1. 版本与来源

- 仓库：https://github.com/AndreaOrru/LaiNES （master 分支）
- 获取方式：`git clone --depth 1`
- Commit：`1f3d126171104ec57e515641ea3b5291d383ebdf`（short `1f3d126`，2025-11-05，"Updated README"）
- 本地路径：`C:\Users\tom\Documents\test\x2nes\core\LaiNES\`
- 上游构建系统：SCons（`SConstruct`），clang++，`-std=c++14 -O3 -march=native`

### 许可证（重要，与任务假设略有出入）

| 部分 | 许可证 |
|---|---|
| LaiNES 本体（src/，`LICENSE` 文件） | **BSD 2-Clause**（Copyright (c) 2020, Andrea Orru）。注意：不是 MIT——任务描述里写 MIT，实际 LICENSE 文件是 BSD-2。两者都允许闭源移植，但需保留版权与免责声明。 |
| `lib/`（blargg Nes_Snd_Emu 0.1.7，Shay Green） | **GNU LGPL**（见 `lib/include/Nes_Apu.h` 头部）。静态链接进 APK/固件时需遵守 LGPL（提供可重链接的目标文件或源码）。移植决策时必须考虑。 |
| `simpleini/`（git 子模块，brofield/simpleini） | MIT。本克隆未初始化子模块（目录为空）；只有 `src/config.cpp` 用它，而 config 属平台层会被替换，可整体丢弃。 |

## 2. 文件清单与分类

### 2.1 纯核心（无 SDL 依赖，直接复用）

| 文件 | 行数 | 说明 |
|---|---|---|
| `src/cpu.cpp` + `src/include/cpu.hpp` | 908 + 41 | 6502 CPU，含全部官方+非法指令、NMI 边沿检测、OAM DMA、开总线行为 |
| `src/ppu.cpp` + `src/include/ppu.hpp` + `src/palette.inc` | 565 + 123 + 11 | PPU、调色板（含 8 组色彩强调预计算表 nesRgb） |
| `src/apu.cpp` + `src/include/apu.hpp` | 113 + 21 | APU 包装层：持有全局 `Nes_Apu apu; Blip_Buffer buf;` |
| `src/cartridge.cpp` + `src/include/cartridge.hpp` | 166 + 30 | iNES 装载、mapper 工厂（switch 实例化）、PRG/CHR 访问路由 |
| `src/mapper.cpp` + `src/include/mapper.hpp` | 108 + 60 | Mapper 基类：PRG/CHR 页表映射、CHR-RAM、存档状态接口 |
| `src/joypad.cpp` + `src/include/joypad.hpp` | 58 + 11 | 手柄移位寄存器 + strobe 语义 |
| `src/savestate.cpp` + `src/include/savestate.hpp` | 271 + 62 | 即时存档（只用 cstdio/cstring/iostream，无 SDL） |
| `src/mappers/*.cpp` + `src/include/mappers/*.hpp` | 见 §4 | 各 mapper 实现 |
| `lib/Blip_Buffer.cpp`、`lib/Multi_Buffer.cpp`、`lib/Nes_Apu.cpp`、`lib/Nes_Oscs.cpp`、`lib/Nes_Namco.cpp`、`lib/Nes_Vrc6.cpp`、`lib/Nonlinear_Buffer.cpp`、`lib/apu_snapshot.cpp` | 398/201/417/498/160/231/189/124 | blargg 音频库（LGPL），纯 C++ 无平台依赖 |
| `lib/include/` | 若干头 | blargg 库头文件（含 `boost/` 兼容头目录） |

核心 + mappers + lib 合计约 6200 行（src 部分），量级非常小。

### 2.2 SDL2 平台层（X2D 移植时整体替换）

| 文件 | 行数 | 说明 |
|---|---|---|
| `src/main.cpp` | 11 | 入口：`GUI::load_settings() → GUI::init() → GUI::run()` |
| `src/gui.cpp` + `src/include/gui.hpp` | 476 + 35 | SDL2 窗口/渲染/音频队列/事件循环/菜单装配；**核心回调的实现侧** |
| `src/menu.cpp` + `src/include/menu.hpp` | 316 + 116 | 菜单 UI（SDL_ttf 文字、vector/functional），还依赖 `dirent.h` 列目录 |
| `src/config.cpp` + `src/include/config.hpp` | 164 + 42 | INI 设置（SimpleIni 子模块）+ 按键映射；头文件里暴露 `SDL_Scancode` 类型 |
| `lib/Sound_Queue.cpp` + `lib/include/Sound_Queue.h` | 138 + ~60 | blargg 的 **SDL 音频输出队列**（SDL_OpenAudio + 信号量三缓冲），属平台层而非核心 |

### 2.3 其它

- `res/`：font.ttf、init.png、laines_icon.png（仅菜单/窗口用，可丢）
- `SConstruct`、`.gitmodules`（simpleini 子模块，未初始化）

## 3. 核心与平台层的接口耦合点（移植关键）

核心对平台的全部依赖收敛在 **`GUI::` 命名空间的 5 个函数** 上（声明于 `src/include/gui.hpp`，实现在 `src/gui.cpp`）。核心代码（cpu/ppu/apu/joypad/cartridge/mappers/savestate）通过调用这 5 个函数完成所有 I/O：

```cpp
namespace GUI {
    u8  get_joypad_state(int n);                        // 输入轮询（核心 → 平台）
    void new_frame(u32* pixels);                        // 视频帧输出（核心 → 平台）
    void new_samples(const blip_sample_t* samples, size_t count); // 音频输出（核心 → 平台）
    bool is_fast_forward();                             // 快进标志查询（核心 → 平台）
    // 以及 init()/run()/toggle_pause()/set_paused() 等纯平台函数（核心不调用）
}
```

### 3.1 启动顺序（自写平台层需复刻的时序）

```
Cartridge::load(path):
    读文件 → 解析 iNES 头（mapperNum = (rom[7]&0xF0)|(rom[6]>>4)）
    → new MapperX(rom)（switch 工厂，见 §4）
    → PPU::reset()          // 必须先于 CPU::power()，因为 power 会 tick PPU
    → APU::reset()
    → CPU::power()          // 清状态 + INT<RESET>()
```

`APU::init()` 只需在进程生命周期调一次（设置 96kHz Blip_Buffer、挂 DMC reader、
IRQ notifier）；`Cartridge::load` 每次换卡/复位都可重入（先 delete 旧 mapper）。

### 3.2 主循环 / 帧驱动

平台层每帧调用一次：

```cpp
CPU::run_frame();   // 执行 TOTAL_CYCLES = 29781 个 CPU 周期 ≈ 1 帧
```

- 内部：`remainingCycles += 29781; while (remainingCycles > 0) exec();`
- 每个 CPU 周期 = **3 次 `PPU::step()`**（`tick_single()` 交错时序，支持子周期
  精度的开总线/DMA 行为）。
- 帧末按固定顺序收尾音频（顺序敏感，核心内部自理，平台无需关心）：
  `APU::run_frame → Cartridge::run_mapper_audio → Cartridge::end_mapper_audio_frame → APU::end_buffer_frame`。
- 参考节拍：LaiNES 的 SDL 层用 `SDL_GetTicks()` 把循环钳到 60 FPS；X2D 平台
  用自己的 vsync/定时器钳 60（1.007...）Hz 即可，核心本身不依赖墙钟。

### 3.3 视频帧获取

- 帧缓冲：**全局 `u32 PPU::pixels[256*240]`**（`src/ppu.cpp:18`）。
- 交付时机：核心在每帧扫描线进入 POST（第 240 行）dot 0 时主动调用
  `GUI::new_frame(PPU::pixels)`（`src/ppu.cpp:419`）——即一帧画面渲染完的瞬间。
- 像素格式：**u32，0x00RRGGBB 布局（alpha 字节恒为 0）**，256×240。
  SDL 侧用 `SDL_PIXELFORMAT_ARGB8888` + 默认无混合所以 alpha=0 无碍。
  **X2D/Android 注意**：若目标 surface 需要 alpha（GL RGBA 纹理开 blend、或
  ANativeWindow RGBA），拷贝时须 `| 0xFF000000` 补 alpha，否则整帧透明。
- new_frame 回调返回后核心继续写同一缓冲（下帧渲染），平台层应在回调里同步
  拷贝（约 240KB/帧），或双缓冲。

### 3.4 音频获取

- 格式：**单声道 16-bit（`blip_sample_t` = short），采样率 96000 Hz**，
  时钟域 1789773 Hz（`APU::init`：`buf.sample_rate(96000); buf.clock_rate(1789773)`）。
- 交付时机：每帧末 `APU::end_buffer_frame()` 中，`buf.samples_avail() >= 4096`
  时读出至多 `OUT_SIZE = 4096` 个样本，调用
  `GUI::new_samples(outBuf, count)`。即**每帧最多一次回调、最多 4096 样本**
  （~42.7ms @ 96kHz；正常每帧应产出 ~1600 样本，>=4096 才触发，节奏约为
  每 2-3 帧一次 4096 块——以 Blip_Buffer 实际 avail 为准，回调里按 count 消费即可）。
- 快进：`GUI::is_fast_forward()` 为 true 时核心会在帧末 `buf.clear()` 丢弃积压，
  平台层在快进时也应不写入音频队列。
- 上游消费端是 `lib/Sound_Queue.cpp`（SDL_OpenAudio 96kHz + 3 缓冲信号量）。
  X2D 移植用 AAudio/Oboe/AAudio 类替代：自建无锁环形缓冲，emu 线程生产、
  音频线程消费，采样率建议直接 96k（或改 `buf.sample_rate()` 后重配——
  那行在 `APU::init`，属核心文件，若不想改就迁就 96k）。

### 3.5 手柄输入注入

- 核心通过 `GUI::get_joypad_state(int n)`（n = 0/1）**轮询**按钮状态，无回调、
  无事件队列；由 `src/joypad.cpp` 在 $4016 strobe 1→0 有效跳变时采样锁存、
  strobe 高电平期间直读 A 键。
- 返回 u8 位域（`src/gui.cpp:269` 明确）：
  `bit0=A, bit1=B, bit2=Select, bit3=Start, bit4=Up, bit5=Down, bit6=Left, bit7=Right`（1=按下）。
- X2D 接法：把相机物理按键映射进一个 `u8`，在 `get_joypad_state` 实现里返回
  即可；线程上它只在 emu 线程内被调用，读一个原子/volatile u8 就够。

### 3.6 头文件级 SDL 耦合（唯一的"编译期"耦合）

- `src/include/gui.hpp` include 了 `<SDL2/SDL.h>`（还声明 `SDL_Texture*`、
  `SDL_Scancode` 返回值——但那些函数只有 menu.cpp/gui.cpp 用）。
- `src/include/config.hpp` include `<SDL2/SDL.h>`、用 `SDL_Scancode`。
- **解法**：自建平台层时提供一个替代 `gui.hpp`（放进我们的 include 路径优先于
  `src/include`），只保留核心真正调用的 5 个原型（§3 开头代码块），去掉 SDL
  include；或更简单——保留上游 gui.hpp 但给编译器一个空 SDL 头垫片。推荐前者：
  核心实际只引用 `GUI::new_frame/new_samples/get_joypad_state/is_fast_forward`。
- 核心源文件里对 `gui.hpp` 的 include 点：`cpu.cpp`（无直接调用）、`ppu.cpp`
  （new_frame）、`apu.cpp`（new_samples/is_fast_forward）、`joypad.cpp`
  （get_joypad_state）、`savestate.cpp`（无）。

### 3.7 存档（可选功能）

`SaveState::save(path)/load(path)` 纯核心实现，序列化 CPU/PPU/APU/mapper 状态。
平台层只需在菜单里调它，不需要 SDL。默认文件名基于 ROM 路径派生。

## 4. Mapper 支持清单

工厂在 `src/cartridge.cpp:81-100`（switch）：

| mapper | 板卡/芯片 | 源文件（cpp+hpp 行数） | 备注 |
|---|---|---|---|
| 0 | NROM | mapper0.hpp（13，纯头文件） | |
| 1 | MMC1 | 86 + 39 | |
| 2 | UNROM | 42 + 34 | |
| 3 | CNROM | 52 + 36 | |
| 4 | MMC3 | 87 + 64 | IRQ + scanline 信号 |
| 5 | MMC5 | 762 + 266 | 最大文件；直接访问 `PPU::ciRam/dot/mask/ctrl` 等内部（ppu.hpp 特意暴露） |
| 7 | AxROM | 50 + 31 | |
| 9 | MMC2 | 104 + 59 | |
| 10 | MMC4 | 102 + 59 | |
| 11 | Color Dreams | 36 + 32 | |
| 24 | VRC6a | 279 + 118 | **含 VRC6 扩展音频**（lib/Nes_Vrc6） |
| 26 | VRC6b | 复用 Mapper24(rom, true) | 同上 |
| 34 | BNROM/NINA-001 | 66 + 43 | |
| 66 | GxROM | 36 + 32 | |

不支持的 mapper 走 default 分支：stderr 报 "mapper N not supported" 并 return
（**注意：此时 rom 内存泄漏且 mapper 为旧值——载入失败检查 `Cartridge::loaded()`**）。

## 5. 全局单例 / 静态状态（决定平台层怎么接）

LaiNES **没有类封装的 Core 对象**，整个模拟器状态是命名空间级全局变量，
**单实例、非线程安全（emu 必须单线程驱动）**：

| 命名空间 | 全局状态 |
|---|---|
| `CPU`（src/cpu.cpp:13-53） | `ram[0x800]`、`A/X/Y/S/PC/P`、`nmi_*`、`irq`、`data_bus`、put/RMW 周期标志、OAM DMA 状态、`remainingCycles`、`ppu_sub_cycle` |
| `PPU`（src/ppu.cpp:8-30） | `pixels[256*240]`（帧缓冲）、`ciRam[0x800]`、`cgRam[0x20]`、`oamMem[0x100]`、`oam/secOam`、`vAddr/tAddr/fX`、`ctrl/mask/status`、`scanline/dot`、`nesRgb[8][64]` |
| `APU`（src/apu.cpp:8-12） | `Nes_Apu apu;`（blargg 对象，自含静态表）、`Blip_Buffer buf;`、`outBuf[4096]` |
| `Cartridge`（src/cartridge.cpp:24-26） | `Mapper* mapper`、`currentRomPath`、`currentMapperId` |
| `Joypad`（src/joypad.cpp:6-8） | `joypad_bits[2]`、`strobe`、`strobe_was_set_by_put` |
| `GUI`（src/gui.cpp，平台层） | SDL 窗口/纹理/菜单指针、`pause`、`fast_forward` |

含义：X2D 平台层就是一个"全局驱动器"——init 后在自己的线程里循环
`CPU::run_frame()`，靠 4 个 GUI 回调收发数据；不存在多实例、也无需加锁
（音频回调线程只读平台层自己的环形缓冲）。blargg `Nes_Apu` 内部有少量
静态常量表，线程安全方面与单线程驱动兼容。

## 6. 编译注意事项（Android NDK / libc++_static）

1. **C++ 标准**：上游 SConstruct 用 `-std=c++14`。C++17 编译应当无碍
   （无 auto_ptr/throw() 之类古董；`Nes_Oscs.h` 等老代码是 C++98 风格但干净）。
   用 C++17 或 C++14 均可，无需 C++20。
2. **STL 依赖（很轻）**：核心实际用到的标准头只有 `<cstdint> <cstring>
   <cstdio> <cstdlib> <iostream> <string>`；`<vector> <functional> <algorithm>
   <csignal> <cctype>` 只出现在平台层（menu/gui/config）。libc++_static 全覆盖。
3. **POSIX 头**：`dirent.h unistd.h sys/stat.h` 只在 menu.cpp/config.cpp
   （平台层，被替换）。纯核心无 POSIX 依赖，可移植性好。
4. **需要排除/替换的文件**：`main.cpp gui.cpp menu.cpp config.cpp
   lib/Sound_Queue.cpp`；同时提供自有 `gui.hpp`（§3.6）与 `GUI::` 5 函数实现。
   其余 src/ + lib/（除 Sound_Queue）原样编入。
5. **不用改动核心即可工作的最小平台层**：
   `GUI::get_joypad_state`（读按键位图）、`GUI::new_frame`（拷贝 256×240×4B，
   补 alpha 0xFF）、`GUI::new_samples`（写环形缓冲）、`GUI::is_fast_forward`
   （常返 false 即可）。
6. **异常/RTTI**：核心未见 try/catch、dynamic_cast；如 NDK 侧想关异常
   （-fno-exceptions）需处理 `new` 失败路径（核心大量裸 new，正常堆大小无碍，
   保守起见建议保留默认异常开启）。
7. **性能**：上游 `-O3 -march=native`；NDK 侧 `-O3` + 对应 ABI（arm64-v8a）即可。
   LaiNES 以精简著称，单核 60fps 余量很大。
8. **内存**：Mapper 构造 `new u8[romSize]`（整个 ROM 副本）+ PRG-RAM 8KB 起 +
   CHR-RAM 8KB；一帧像素 240KB。峰值内存 = ROM 大小 + 数百 KB，X2D 无压力。
9. **LGPL（lib/）**：静态链接 blargg 库时按 LGPL 要求提供对应目标文件/
   源码获取途径；若不可接受，可评估整体替换 APU 为其他实现（工作量大，
   不建议）。LaiNES 本体 BSD-2 只需保留版权声明。
10. **C++ 一次性初始化**：`APU::init()` 在 `GUI::init()` 里被调（96000Hz）；
    自有平台层在启动时调一次 `APU::init()`（或自行设置采样率后再 init——
    采样率硬编码在 apu.cpp `buf.sample_rate(96000)`，不改核心就只能 96k）。

## 7. 附：testrom 验证记录

- `nestest.nes`：24,592 B = 16(iNES头)+16KB PRG+8KB CHR，头 `4E 45 53 1A`，
  PRG=01 CHR=01，flags6/7=0 → mapper 0 水平镜像。来源：
  github.com/christopherpow/nes-test-roms（模拟器开发者社区公测合集）。
  用途：CPU 指令级正确性（自动模式 $C000 起，日志对比）。
- `240pee.nes`（240p Test Suite for NES v0.23，作者 Damian Yerrick/pinobatch，
  官方 GitHub Release 公开发布）：65,552 B = 16+64KB PRG+0 CHR(CHR-RAM)，
  头 `4E 45 53 1A 04 00 21 08` → mapper 2（UNROM），垂直镜像，NES 2.0 头。
  两者的 mapper（0、2）LaiNES 均支持。均为明确公开分发，非任天堂商业 ROM。
