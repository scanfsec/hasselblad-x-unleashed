# hblProductInfo "Fatal, Unknown product" 逆向笔记

对象：`/system/bin/camera-gui`（aarch64 PIE，符号完整，NDK r16b，Qt/libc++ 静态链接）
症状：LD_PRELOAD 手动启动时 `Version::hblProductInfo::hblProductInfo()` 内 abort：
`hblversion.cpp:0353 Version::hblProductInfo::hblProductInfo(): Fatal, Unknown product`

## 0. 结论速览

- **数据源是文件：`fopen("/proc/cmdline", "rb")`**，不是 property / getenv / DBus。
- 内容用 libc++ 静态链接的 `std::regex` 搜索 **`hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})`**（点号未转义，匹配任意字符）。
- 第 1 组 = product id，第 2 组 = hw revision，`strtol` 十进制转换。
- **合法 product id：4..14**（构造函数 `(unsigned)(id-4) <= 10` 判定）。
- 真机内核 cmdline（crashlogs dmesg 实测）含 **`androidboot.hw_version=06.04.01.04`** → id=6（"X2D II 100C"，即 splashboot=hb722），hwRev=4。
- **绕法：在 LD_PRELOAD .so 里导出 `fopen`，对 `"/proc/cmdline"` 返回 `fmemopen()` 的假 FILE***，内容至少含 `hw_version=06.04.01.04`。读路径全走 stdio（fopen→fread→fclose，全部 PLT 可截获），fmemopen 的 FILE* 完全兼容。

## 1. 符号与调用链（tombstone 地址全部对上）

| 符号 | 地址 |
|---|---|
| `Version::hblProductInfo::instance()` | 0x179f0c8（tombstone #06: +0x28 = 0x179f0f0 = `bl ctor`） |
| `Version::hblProductInfo::hblProductInfo()` C1/C2 | 0x179f540（#05: +316 = +0x13C = 0x179f67c = `bl QMessageLogger::fatal`） |
| `Version::hblProductInfo::m_instance` (BSS) | 0x2b4ec88 |
| `GuiObjectImpl::joystick_move_focus() const` | 0x110b5b8（#07: +36 = 0x110b5dc = `bl instance()`） |
| `producttype_get_product_id` | 0x17a3288 |
| `producttype_get_hw_revision` | 0x17a4068 |
| `productInfo()`（匿名 ns，producttype.cpp） | 0x17a32e8 |
| `str2int(char const*)` | 0x17a40c8 |
| `gCmdLinePath` (std::string 静态，producttype.cpp) | 0x2b4f178 |
| `productInfo()::prodType` / `::hwRev` | 0x2b4f190 / 0x2b4f198（各 int+1 ok 字节：0x2b4f194 / 0x2b4f19c） |
| `gMutex` | 0x2b4f150 |
| `_GLOBAL__sub_I_producttype.cpp` | 0x409e78 |

```
joystick_move_focus()                        0x110b5b8
  GuiProxies::camera()->vtable[+1208]()      启动早期返回 false
  → hblProductInfo::instance()               0x179f0c8   m_instance==NULL → new(8) → ctor
    → hblProductInfo::hblProductInfo()       0x179f540
        w19 = producttype_get_product_id()   0x17a3288  (gMutex + productInfo)
        w0  = producttype_get_hw_revision()  0x17a4068
        if ((u32)(w19 - 4) > 10) → warning("Unknown product type: N") + fatal("Fatal, Unknown product")
        else 对象 = {int productId; int hwRev;}   // 8 字节，stp w0, w19
```

对象只有 8 字节：`[0]=hwRev(int)`，`[4]=productId(int)`（`stp w0, w19, [x20]`）。
`Version::hardwareID()`（0x179f120）复用同一个 m_instance 单例——修好构造即全部通路修好。

### 构造函数关键反汇编（0x179f540）

```asm
179f570: bl   17a3288 <producttype_get_product_id>
179f574: mov  w19, w0
179f578: bl   17a4068 <producttype_get_hw_revision>
179f57c: sub  w8, w19, #0x4          ; id - 4
179f580: cmp  w8, #0xa               ; > 10 ?
179f584: b.hi 179f5b4                ; → warning + fatal
179f588: stp  w0, w19, [x20]         ; 存 {hwRev, productId}
...
179f5e8: bl   QMessageLogger::warning()      ; "Unknown product type: " << id (QDebug)
...
179f67c: bl   29b094 <QMessageLogger::fatal(char const*, ...) const>   ; ← 崩点 +316
        ; fatal 格式串 0x1cd1787 = "Fatal, Unknown product"
        ; QMessageLogger 上下文: file=0x1cd147d ".../appscommon/hblversion.cpp"
        ;                       func=0x1c898e2 "Version::hblProductInfo::hblProductInfo()"
        ;                       line=353 (0x161，在 0x1d196d0 的 d0 双字里)
```

## 2. 数据源全程：productInfo()（0x17a32e8）

`producttype_get_product_id`：`gMutex.lock()` → `productInfo()` → `w0=低32位=id`，`w1=有效标志`；无效返回 255。`get_hw_revision`：高32位，无效返回 -1。

`productInfo()`（首次调用解析，之后命中 0x2b4f190/198 缓存 + 0x2b4f19c ok 字节直接返回 `(hwRev<<32)|prodType`, w1=1）：

```asm
17a347c: add  x1, x1, #0xe3e      ; x1 = "rb"        (0x1cd1e3e)
         mov  x0, x21             ; x0 = gCmdLinePath.c_str()
17a3488: bl   29a3f0 <fopen@plt>  ; fopen("/proc/cmdline", "rb")
17a348c: str  x0, [sp, #344]      ; 存进 basic_filebuf::__file_
17a3490: cbz  x0, 17a33c8         ; NULL → "Failed to open" + path（cerr）+ failbit
17a349c: ...  operator new(0x800) + memset + istream::read(buf, 2048)
17a34d0: cmp  w8(gcount), #1 ; < 1 → "Failed input-read: " + strerror(errno)
         ; 之后 std::string content(buf, gcount); fclose@plt
17a36e8: 正则字面量 0x1cd1d9e = "hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})"
         basic_regex::__parse + regex_search(content, m)
17a3920: tbz  w19, #0 → "No or too few matches found"   ; !matched
17a3934: cmp  size, #2 → 同上                            ; m.size() < 2
17a3944: m[1] → std::string → str2int → str w0,[0x2b4f190]; ok byte [0x2b4f194]
         ; !ok → "Failed str conversion, prodType: " + m[1]
17a3d04: m[2] → str2int → [0x2b4f198] / ok [0x2b4f19c]
         ; !ok → "Failed str conversion, version, hwRev: " + m[2]
17a3320: 成功: w1=1; x0 = (hwRev<<32) | prodType
```

`str2int`（0x17a40c8）= `strtol(s, &end, 10)`；`end==s` 或 errno==ERANGE（"Over/under-flow in: " / "No valid number found in: "）→ 失败。返回 `x0 = (ok?1:0)<<32 | value`。

`gCmdLinePath` 由 `_GLOBAL__sub_I_producttype.cpp`（0x409e78）静态初始化为 SSO 字面量 **`/proc/cmdline`**（0x1cd1460，13 字符；首字节 26=13<<1 是 libc++ SSO 长度编码）。nm 里无任何 setter 引用该全局 —— **运行期不可被 env/property 改写**。（hblversion.cpp 的 `Version::readCmdline/setCmdlineFile`、缓存 0x2b4ec90 是固件版本显示用的另一套，与产品判定无关。）

### 读路径确认全 stdio
libc++ 静态链接的 `basic_filebuf<char>::underflow()`（0x17a4b68）内部是 `bl fread@plt`（0x17a4c88/0x17a4d1c），打开用 `fopen@plt`，关闭 `fclose@plt`。**没有裸 read()/openat() 调用** —— 这意味着：
1. 我们已有的 openat hook 拦不到这条链（bionic fopen 在 libc 内部直接走 openat syscall，不经过可抢占的 openat 符号）；
2. 但 `fopen` 是 PLT 动态符号，LD_PRELOAD 导出同名函数即可抢占。

## 3. hblversion.cpp:353 的语义 + 产品 ID 表

353 行逻辑（从引用字符串复原）：

```cpp
Version::hblProductInfo::hblProductInfo() {
    unsigned id = producttype_get_product_id();     // /proc/cmdline 里 hw_version= 第1组
    unsigned rev = producttype_get_hw_revision();   // 第2组
    if (id - 4 > 10 /*unsigned*/) {                 // 合法: 4..14，其余(含255)全部 fatal
        qWarning() << "Unknown product type:" << id;
        qFatal("Fatal, Unknown product");           // ← 行353
    }
    productId = id; hwRevision = rev;
}
```

`Version::productName(Version::ProductID)`（0x179eb30，同一 `id-4≤10` 判定 + 跳转表 @0x26797c8，元素=偏移/4，0x22→default）：

| id | productName (0x179eb7c 系) | 备注 |
|---|---|---|
| 4 | "X2D 100C" | |
| 5 | "CFV 100C" | |
| **6** | **"X2D II 100C"** | **本机（hw_version=06.04.01.04）** |
| 7–11 | （合法但 "Unhandled model:"） | 预留 |
| 12 | "X1D II 50C" | |
| 13 | （合法但 Unhandled） | |
| 14 | "CFV II 50C" | |

modelNumber 相关串：`HB722`、`X2DII-SSD`、`X2DII-CFe`、`X2D-100C-SSD`、`CFV-100C-CFe` 等（0x1cd1640–0x1cd16c0）。
真机 dmesg（crashlogs）：`Kernel command line: ... splashboot=hb722 ... androidboot.hw_version=06.04.01.04 ...`
→ 正则命中 `hw_version=06.04.01`（`hw_version=` 是 `androidboot.hw_version=` 的后缀，search 无锚点，直接命中），id=6, hwRev=4，构造成功。

**我们崩的原因**：非 service 环境下上述三段式（open → read ≥1 字节 → 正则命中且两组均可转数字）任一失败 → `productInfo()` 返回 w1=0 → get_product_id 返回 255 → fatal。启动 stderr 里应能看到对应前兆（QMessageLogger::warning 同样会打 "Failed to open"/"Failed input-read"/"No or too few matches found" 之一，可据此定位具体断点）。

## 4. 伪造方案（LD_PRELOAD，-nostdlib，extern 声明）

**Hook 点：`fopen`（libc.so 动态符号，PLT 抢占）。返回假 `/proc/cmdline`。**

```c
/* extern 声明，链接期/运行期都解析到 libc.so */
extern void *fdopen(int fd, const char *mode);
extern void *fmemopen(void *buf, unsigned long size, const char *mode);

/* 与真机一致的假 cmdline（最短只要含 hw_version=NN.NN.NN 即可，NN₁∈[04..14]） */
static const char FAKE_CMDLINE[] =
    "console=secS0 androidboot.hardware=eagle2 splashboot=hb722 "
    "mp_state=production androidboot.hw_version=06.04.01.04 "
    "androidboot.slot_suffix=1 quiet\n";
/* 注意: 需 NUL 结尾给 fmemopen 用 size=sizeof-1 */

void *fopen(const char *path, const char *mode) {
    if (str_eq(path, "/proc/cmdline"))            /* 手写比较，-nostdlib */
        return fmemopen((void*)FAKE_CMDLINE, sizeof(FAKE_CMDLINE)-1, "r");
    /* 其余路径转发: mode 首字符 r/w/a → openat + fdopen */
    int flags = (mode[0]=='r') ? 0 : (mode[0]=='w') ? 0x241/*O_WRONLY|O_CREAT|O_TRUNC*/ : 0x441;
    long fd = my_openat(AT_FDCWD, path, flags | 0x80000/*O_CLOEXEC*/, 0644); /* 走已有 syscall 封装 */
    if (fd < 0) return 0;
    return fdopen((int)fd, mode);
}
```

要点：
- **值**：`06.04.01.04` → id=6（X2D II 100C）/ hwRev=4，与真机一致，`hasJoystick` 等后续分支行为同原生。想伪装其他机型改第 1 组数字即可（4/5/6/12/14 有名字，7–11/13 无名但合法）。
- **fmemopen 兼容性**：后续 `istream::read(2048)` 走 `fread@plt`（underflow 实测），gcount=len>0 ✓；结束时 `fclose@plt` 对 fmemopen FILE* 合法 ✓。bionic 自 API 23 有 fmemopen（本机 Android 27）。
- **不要**去 hook `__system_property_get`/`getenv`/DBus —— 这条链根本不碰它们。
- **不能**hook `producttype_get_product_id` 本身：它是主程序内部符号（0x17a3288，非 PLT），直接 `bl`，LD_PRELOAD 无法拦截。
- 缓存效应：`productInfo()` 首次成功后结果进静态缓存（0x2b4f190/198），同一进程内后续读取不再碰文件；fopen hook 只需在最早一次 joystick/GUI 事件前生效（LD_PRELOAD 从进程启动就在，天然满足）。
- 备选（若想更"真"）：hook `fopen` 时把假内容写进真实临时文件（openat O_CREAT）再 `fdopen` 返回 —— 完全走内核文件路径，兼容任何 stdio 实现细节。
- 另一个根因排查方向：若 stderr 前兆是 "Failed to open"，说明现有 openat hook/环境把 /proc 弄坏了（例如挂载命名空间里没有 /proc）——直接放行 `/proc/*` 或在 fake 内容里用上面的 fmemopen 都能解。

## 5. joystick_move_focus 触发条件（可选防御，结论：治不了本）

```asm
110b5c8: bl   GuiProxies::camera()
110b5d0: ldr  x8, [x8, #1208]   ; camera proxy 虚方法（某设置项，启动早期为 false）
110b5d4: blr  x8
110b5d8: tbnz w0, #0, 110b5e8   ; true → 跳过产品查询
110b5dc: bl   instance()        ; ← 崩在这里（首次构造单例）
110b5e0: bl   hasJoystick()
110b5e4: tbz  w0, #0 → return 0
110b5e8: bl   GuiProxies::system() (+1176) …
```

启动期输入事件分发里任何 joystick 事件都会走到这里；`m_instance` 只要有一次构造成功就永久缓存。从源头避免（吞 joystick 事件）不可靠——`hardwareID()` 等其他路径也会构造同一单例。**正解就是喂对 /proc/cmdline。**

## 6. 关键字符串地址备查

| 地址 | 内容 |
|---|---|
| 0x1cd147d | `/home/build/jenkins/default/hardware/hbl/cam_fw/linux/libs/appscommon/hblversion.cpp`（QMessageLogger file） |
| 0x1c898e2 | `Version::hblProductInfo::hblProductInfo()`（QMessageLogger func） |
| 0x1cd1770 | `Unknown product type: `（warning 前缀） |
| 0x1cd1787 | `Fatal, Unknown product`（fatal 格式串） |
| 0x1cd1460 | `/proc/cmdline`（gCmdLinePath 默认值） |
| 0x1cd1d9e | `hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})`（正则，点未转义） |
| 0x1cd1e3e | `rb`（fopen mode） |
| 0x1c8afc3 | `Failed to open`（14 字节，fopen 失败时输出） |
| 0x1cd1e2a | `Failed input-read: `（19 字节） |
| 0x1cd1e0e | `No or too few matches found`（27 字节） |
| 0x1cd1dd0 | `Failed str conversion, prodType: `（32 字节） |
| 0x1cd1dfc | `Failed str conversion, version, hwRev: ` |
| 0x1cd1e41 | `No valid number found in: `（str2int） |
| 0x1cd1e5c | `Over/under-flow in: `（str2int） |
| 0x1cd1600 区 | `X1D II 50C`/`CFV II 50C`/`CFV 100C`；0x1cd08df `X2D 100C`；0x1cd08e8 `X2D II 100C`；`HB722`、`X2DII-SSD` 等 |
