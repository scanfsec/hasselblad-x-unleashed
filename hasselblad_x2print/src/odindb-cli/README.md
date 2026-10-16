> **定位：odindb-cli 是 odindb-send 的测试移植/一比一复刻验证项目**（对拍基础设施：tools/run_odindb_conformance.sh + qemu 原版双实现比对）。**不随产品编译部署**（service.sh 的 install 不含它；验收用相机原版 /system/bin/odindb-send）。本目录与 build_odindb.py/build_wsl_odindb.sh 保留作逆向档案。

# odindb-cli — 哈苏 odindb-send 的一比一复刻(通用重实现)

纯 C11 + libdbus 的命令行客户端, 从零重写工厂 CLI `odindb-send`
(`/system/bin/odindb-send`, v10.00.09.77), 替代 `tools/patch_odindb_send.py`
的二进制补丁路线。

**默认路径与原厂一比一**: 内置已知模块表 = 原版 16 模块 + printer,
槽位面(属性子集与默认值/方法签名/信号过滤/列表输出)逐字采集自 qemu 下
原版实测(`faces.c`, 610 属性/全方法/全信号), 核心命令矩阵 stdout 与退出码
100% 一致(`tools/odindb_cli_conformance.md`)。`--any` 逃生旗标可显式开启
任意模块名(自省通用路径, 供未来新服务)。

## 语法(严格复刻原版 usage)

```
odindb-cli -s -l                                # 列服务(内置表: 原版 16 + printer)
odindb-cli --list-services
odindb-cli -s <mod> -m -l                       # 列方法(面表原文)
odindb-cli -s <mod> -p -l                       # 列属性
odindb-cli -s <mod> -p -l -i                    # 属性详情(Name/Type)
odindb-cli -s <mod> -m -l -i                    # 方法详情(In/Out 参数, 枚举展开)
odindb-cli -s <mod> --list-signals
odindb-cli -s <mod> --list-all                  # 属性→方法→信号(原版顺序)
odindb-cli -s <mod> --get-all                   # 属性+当前值(面子集; 原版怪癖一并复刻)
odindb-cli -s <mod> -p <prop>                   # 读属性(面内; GetAll 缓存)
odindb-cli -s <mod> -p <prop> <value>           # 写属性(类型取自省)
odindb-cli -s <mod> -p <prop> -- -60            # 负值
odindb-cli -s <mod> -p <prop>:<数值mask>        # 位读 → 0/1
odindb-cli -s <mod> -m <method> [args...]       # 方法调用(面签名编码; 去 do 前缀)
odindb-cli -s <mod> -m <method> ... 30          # 末位多余参数 = timeout 秒(默认 25)
odindb-cli -s <mod> --signal a,b|all            # 持续监听(面信号过滤, Ctrl^C 退出)
odindb-cli -s <mod> --signal-oneshot <name>     # 收到一帧即退
odindb-cli -s <mod> --trigger-memunreachable    # 调 triggerMemunreachableLog(u)
odindb-cli --acquire-wakelock / --release-wakelock   # 经 com.hasselblad.system
odindb-cli -b system|session|none ...           # 选总线(默认 system)
odindb-cli -e <enum> / --list-enums             # 枚举(见差异清单: 不可用)
odindb-cli --any -s <新模块名> ...              # 逃生旗标: 任意模块名(自省通用路径)
odindb-cli -h/-v/-a/--apps-messaging-version/--show-usage/--print-cmdline
```

已知模块表(gui system usbhost storage testd camera error phocus prodinfo
upgrade exmcu settings tcphost hosttransfer wltcphost suc **printer**)之外
的名字: 原版同款拒绝(`No such service: X`, rc=1, 连接后本地判表, 与原版
报文一致); `--any` 绕过判表走 dbus 探测。

### 参数编解码(CLI 文本 → D-Bus 类型, 见 argspec.c)

| 目标类型 | CLI 文本形式 | 例 |
|---|---|---|
| `i`/`n` | 十进制; **位集枚举** `a\|b\|...` 按位 OR(数值分量) | `60`, `1\|2\|4` |
| `u`/`q`/`x`/`t`/`d` | 十进制/浮点 | `42`, `3.14` |
| `b` | `true`/`false`/`1`/`0` | `true` |
| `y` | 0..255 | `3` |
| `s` | 裸字符串 | `/cfe/939HASBL/B0000881` |
| `ay` | 逗号分隔十六进制字节 | `41,42,43` |
| `as` | JSON 数组(原版风格) | `["One","Two"]` |
| `a{sv}` | JSON 对象(原版风格): 值 s/b/i/d | `{"lensModel":"CF 38","av":60}` |
| `v` | 按文本形态自动判型 | — |

方法签名默认取面表; `--any` 路径取 D-Bus 自省。写属性类型取自省
(等价报告的"不确定类型时先探测")。方法调用按 odindb wrapper 约定把
timeout 作为线上末参 `int32` 附带(-1=默认 25s; `-m ... 30` 显式秒数)。

## 与原版的差异清单

1. **printer 是我们加的第 17 表项**(原版 16 槽无 printer 槽, 物理差异):
   `-s -l` 列表在原版 16 名后多一行 `printer`; printer 槽位面数据采集自
   补丁副本(= usbhost 槽面)。
2. **枚举**: 原版内置 HblmTypes 表。本实现: `--list-enums`/`-e` 明确报错;
   位集语法保留(`1|2` 有效), `E_` 名分量不可解析; 枚举型属性在服务**未供值**
   时打印面表默认字面块(与原版逐字一致, 含 `E_Xxx(0)` 形态), 服务**供值**时
   打印 i32 数值(原版会打 `E_Xxx(n)`, 需编译期枚举表, 无法复刻)。
3. **写属性封送**: 原版恒发 `variant string`(QtDBus 服务端自行转换); 本实现
   按自省类型发**类型化 variant**(x2print printer-service 的 Set 校验类型,
   原版封送会被其拒)。QtDBus 服务两端都兼容。面路径写属性需一条自省报文
   定可写性/类型(原版查 wrapper 编译期 WRITE 标志, 零报文)。
4. **`--any` 通用路径**(扩展): 自省驱动, `--get-all` 展示服务全部实际属性
   且 rc=0(不复刻原版 moc 过滤与怪癖), `<prop>Changed` 订阅走
   propertiesChanged 过滤, 未自省到的信号名也照样订阅。默认路径不用它。
5. **退出清理**: 原版 RemoveMatch ×4(含一条 bogus 规则, 总线报错); 本实现
   按实际添加的规则清理。housekeeping 层面差异。
6. **`--apps-messaging-version`/`-a`/`-v`**: 打印本实现版本(x2print-1.0.0),
   非原版 git hash。
7. **退出码**: `com.hasselblad.error.{failed,parametererror,busy,canceled,timeout}`
   → {1,2,3,4,255}; 其余 dbus 错误 → 1。错误文本到 stdout(原版输出亦全在
   stdout, 实测如此)。`-p <prop> -i` 单属性详情未复刻。

## 三个构建目标

| 目标 | 命令 | 产物 | 说明 |
|---|---|---|---|
| Android(相机) | `python build_odindb.py`(Windows) | `build/aarch64/odindb-cli` | NDK r25b aarch64-linux-android21, 链接设备 `inc/lib/libdbus.so`(运行时用相机 /system/lib64), 推到 `/data/x2print/`; 相机 system bus 在 `/dev/socket/dbus`(自动回退, 或设 `DBUS_SYSTEM_BUS_ADDRESS`/`DBUS_SOCKET`) |
| WSL 原生 | `wsl -d Ubuntu-24.04 -- bash build_wsl_odindb.sh` | `build/wsl/odindb-cli` | x86_64 + 系统 libdbus-1, 用于对拍/调试 |
| 对拍验收 | `wsl -d Ubuntu-24.04 -- bash tools/run_odindb_conformance.sh` | `/tmp/odinconf/` + `tools/odindb_cli_conformance.md` | 原版原件(16 原生模块)+补丁副本(printer) vs 本实现, stdout/rc/总线报文三维对比 |

构建均 `-std=c11 -Wall -Wextra -Werror` 零警告。

## 模块

- `main.c` — CLI 入口: 参数解析(usage 全表)、门控+面路径分发、通用路径
- `faces.c/.h` — **生成文件(勿手改)**: 17 模块槽位面数据。再生成流程:
  `wsl ... bash tools/collect_faces.sh`(qemu 采集原版输出) →
  `python3 tools/gen_odindb_faces.py /tmp/faces <x2print根>`。
  固件升级换构建后重跑即可(数据与原版构建耦合, 属设计)。
- `client.c/.h` — libdbus 封装: 三元组模板化、QtDBus 等效启动序列
  (AddMatch propertiesChanged/NameOwnerChanged×2 → GetNameOwner → NameHasOwner
  → GetAll(本模块) → GetAll(linkstatus), 报文级对齐实测序列)、GetAll/Get/Set/
  方法调用/Introspect/自省解析/错误名→退出码
- `argspec.c/.h` — CLI 参数→D-Bus 类型编解码(见上表) + 位集枚举 + JSON 子集
- `dumpfmt.c/.h` — QVariant 风格输出(值/属性行/信号帧/Qt 类型名)

## 设计要点(血泪)

- 属性变更信号 member 是小写 p 的 `propertiesChanged`(线协议关键);
  订阅 match rule 与原版 QtDBus 生成串逐字一致(对拍验证)。
- 读属性不发 `Get`(原版生成代码零 xref), 走 GetAll 缓存——报文级对齐;
  `odin_get` 仅在 GetAll 出错时兜底。
- dict entry 遍历必须用 `get_basic`+`next` 直读, 不能 `recurse` 进 entry
  (libdbus 1.14 实测 SIGSEGV)。
- 阻塞调用(`send_with_reply_and_block`)收到错误回复时返回 NULL 且
  DBusError 置位——原版三行错误样式(`"member" : "msg"`/msg/Error)在该
  分支处理。
- `--signal all` 只给面表 wire 信号加订阅; propertiesChanged 静默
  (原版不打的帧也不打)。
