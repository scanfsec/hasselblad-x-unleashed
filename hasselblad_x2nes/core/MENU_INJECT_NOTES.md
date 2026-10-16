# X2D II (fw v1.3.16.2) 左滑设置菜单 "GAME" 注入点调查

日期: 2026-09-22 ｜ 对象: `system_decrypted/bin/camera-gui` (64,082,728B, aarch64, 符号完整, Qt 6.8.2 静态)
方法: 全量 qrc 树提取 (core/qrc_dump.py, 1112 文件 0 错误) + WSL objdump 反汇编 + .tmpwork/camera-gui.nm 符号表 + 内嵌 .qm 翻译验证
输入: REPORT_O1 (QML 加载链/AOT), REPORT_O3 (菜单表/驱动模式), REPORT_P (LD_PRELOAD 真机先例), analysis/gui_secret_menu.md

---

## 0. 一句话结论

**设置菜单的 11 个分类既不在 QML 里也不是翻译文件驱动——它们是 menux2d.cpp 的 C++ 静态表 (`Menu::topMenus()`)，经 C++ 单例 `FavoriteModel`(QAbstractListModel) 喂给 `MainScreen.qml` 网格；QML 侧无法向 C++ 模型 append。但 `MainScreen_grid.model` 和页面本身都可以在运行时从注入壳里替换/叠加（P2 driveModeModel 同款手法），且启动通道 `System.doRun_debug_script(1..4)` → `/system/bin/custom_debug_%1.sh` 已被官方自定义键下拉原生支持。推荐 = 现有 modqml.so 薄壳 + 运行时在 MainScreen 第 12 个空格位叠加 GAME 按钮 + doRun_debug_script(N)；全量镜像 (改 appPrefix) 为像素级原生的升级路径。**

---

## 1. 设置菜单列表的来源（问题 1 答案）

### 1.1 渲染链（全部实证）

```
main.qml (qrc, 59行薄壳)
 └ MainContent.qml
   └ controlscreen/ControlDrawer.qml          ← 左滑容器: HblListView 2页
       ├ [0] ControlScreen.qml (取景控制屏)
       └ [1] Constants.appPrefix + "mainmenu/MainScreen.qml"   ← 左滑出来的那页
            └ Components.CustomGridView (objectName "MainScreen_grid")
                model: viewModel.favoriteModel          ← C++ QML 单例
                delegate: FramedImage { text: qsTranslate(labelContext, label);
                                        source: iconSrc; onClicked→gotClick() }
                └ gotClick() → root.loadSubMenuItem(menuName,...)   [signal]
                     → ControlDrawer: (item as MainScreen).loadSubMenuItem
                       .connect(mainMenu.loadSubmenu)   (onLoaded 时连接)
                     → MainMenu.loadSubMenuItem(menuName)
                        ├ gate: SubMenu.menuRw(menuName)
                        ├ viewModel.menu.file = SubMenu.menuFile(menuName)  ← C++ map 查 QML 文件
                        └ menu_loader.source = file → SettingsGeneric.qml
                            └ populateModel(): list.model = SubMenu.getModel(level)
```

- **MainScreen.qml** = 分类网格页（不是"收藏"——`FavoriteModel::init()` @0x16f1f80 把
  `Menu::topMenus()` **全部**合法顶层菜单 addItem 进模型，每个再按 conditionValid(avail/rw) 定 itemEnabled）。
- **SettingsGeneric.qml** = 点入分类后的设置列表页，列表模型 = C++ `SubMenu.getModel(level)`（SubMenuModel/FilteredSubMenuModel, menumodel 模块, qmldir `prefer :/qt/qml/...`, C++ 单例注册）。

### 1.2 分类数据在 C++，不在 QML

| 证据 | 地址/文件 |
|---|---|
| `Menu::topMenus()` 门面 → `MenuX2d::topMenus()` 返回静态初始化的 QVector | 0x16f4b68 / 0x1706b40 (返回 bss 0x2b4ce70) |
| `MenuX2d::menuFile(QString)`：QMap<QString,MenuInfo> 查名→QML 文件；**未知名 miss → 返回空 QString**（0x1706d9c `stp xzr,xzr`）| 0x1706ca8 |
| 顶层 Submenu 构造序列（`Submenu::Submenu(QString&, E_MenuOption, bool, E_MenuRW, E_MenuAvail)` @0x1703b08） | menux2d.cpp 静态初始化 0x395724–0x396c34 等 |
| `FavoriteModel`（C++ QML 单例, favoritemodel.cpp）init/reloadModel/addItem | 0x16f1f80 / 0x16f1f48 / 0x16f2530 |
| 11 个分类的 menuName + 图标字面量（UTF-16 成对存放于 .rodata） | 0x1cedb00–0x1cee566 区 |
| 分类中文文案 = 内嵌 .qm 翻译（`qrc:/translations/camera_zh_CN.qm`，"裁切模式/电源/闪光灯/防抖…"全命中），QML 用 `qsTranslate(labelContext,label)` | t02235d0d 树 |

11 个顶层分类（menuName ↔ 图标 ↔ 用户所见）：

| menuName | icon | 中文 |
|---|---|---|
| exposureMenu | ic_menu_exposure | 曝光 |
| focusMenu | ic_menu_focus | 对焦 |
| qualityMenu | ic_menu_quality | 画质 |
| cropModesMenu | ic_menu_crop_modes | 裁切模式 |
| flashMenu | ic_menu_flash | 闪光灯 |
| displayMenu | ic_menu_display | 显示 |
| powerMenu | ic_menu_power | 电源 |
| storageMenu | ic_menu_storage | 储存 |
| ibisMenu | ic_menu_ibis | 防抖 |
| connectionMenu | ic_menu_connection | 连接 |
| generalMenu | ic_menu_general | 通用 |

其余嵌套子菜单名（同区字符串，供参考）：aboutMenu/cameraBodyMenu/customButtonsMenu/customOverlaysMenu/dateTimeMenu/developerMenu/languageMenu/lensSelectionMenu/profilesMenu/serviceMenu(通用→服务, zh 翻译"通用 - 服务 - 传感器校准"实证)/soundMenu/spiritLevelMenu/touchMenu/wifiMenu/usageMenu。

**主 QML 树内设置菜单相关文件全清单**（qrc:/qt/qml/app/qml/ 下，已 dump 到 core/qrc_dump/t021600e8/）：
mainmenu/ 32 文件：MainMenu.qml(240行 滑入加载器) MenuSwipe.qml(143行 拖拽) MainScreen.qml(177行 **分类网格**) MainMenuViewModel.qml(359行) MenuData.qml MenuBase.qml SubMenuBase.qml SettingsGeneric.qml(518行 **列表页**) MenuListView.qml MenuHeader.qml MenuSubdialog.qml ListSelectorSettings.qml SettingSlider.qml MenuBoolSelector.qml CustomButtonsView.qml(459) CustomButtonsItem.qml(297) MyLenses.qml ProfilesView.qml DateTime.qml TimeZoneView.qml LicenseView.qml VibrationsView.qml FirstTimeGuide*.qml + delegates/(SubmenuDelegate/ButtonDelegate/DropdownDelegate/SliderDelegate/DateTimeDelegate/ValueSwitchDelegate) + popups/(MenuPopup/ConfirmResetAllSettings/DiskFormat/ProfileEdit/EvfDiopterSelector/PopoverCombinationLock/RecalibrateSensor)。
外围：components/MenuItemModel.qml、controlscreen/ControlDrawer.qml（左滑容器+MainScreen 装载）、viewmodels/StateMachineViewModel.qml（自定义键处理）。

### 1.3 为什么不能直接改 qrc 里的 MainScreen.qml

O1 §4 已证：qmlcachegen AOT 编译单元对 `qrc:` scheme + 路径全匹配的 URL 恒优先（sourceTimestamp=0 恒接受）——同偏移/同位替换 qrc 内 QML 文本对引擎不可见。可行域只有 (a) file: URL 加载（scheme 失配绕过 AOT）或 (b) 运行时从外部 QML 改对象树。

---

## 2. qrc 全量提取（问题 2 答案，已完成）

- **工具**: `core/qrc_dump.py`（自包含）。枚举 .text 中所有 `BL qRegisterResourceData(0xe90980)` 调用点，回溯 ADRP/ADD 解码 (TREE, NAMES, DATA) 三指针 → 22 字节大端节点树遍历 → zlib 解压 → 按真实路径落盘。
- **rcc 格式实证修正**（本 build，与上游 Qt 不同处已标注）：
  - 节点 22B：`@0 u32 name_off | @4 u16 flags(bit1=Directory, bit0=zlib) | @6 u32 Dir:childcount/File:恒1 | @10 u32 Dir:firstchild/**File:payload_off** | @14 u32 timestamp | @18 u32 checksum`。（.tmpwork/o1/rcc2.py 旧注释把 payload_off 标在 @6 是错的，@6 在文件节点恒为 1。）
  - names 条目 `[u16 len][u32 hash][UTF-16BE]`；payload 条目 plain=`[u32 size][data]`，zlib=`[u32 comp][u32 unc][zlib]`。
  - 路径首组件重复（rcc mount root），URL = `qrc:/` + 去首组件路径。
- **产出**: `core/qrc_dump/` —— **72 棵树 / 1112 文件 / 0 错误**（381 QML + 459 SVG + 66 qmldir + 10 .qm 翻译 + qsb shader 等，9.7MB），`manifest.json` 含每文件 payload 偏移/大小/压缩标志。主 QML 树 = `t021600e8/qt/qml/app/qml/`（346 文件），图标树 `t022cbeb6/icons/`（470），翻译 `t02235d0d/translations/`。
- **验证**: `t021600e8/qt/qml/app/qml/main.qml` 与 `.tmpwork/o1/main.qml`（O1 报告提取件）逐字节一致；主树 346 文件 zlib/plain 全部解压成功且长度校验通过。
- 这就是"全量落地"方案的物料：整个 app QML 树已经可以直接镜像到 /tmp/modqml/。

---

## 3. System.run_debug_script 的 UI 入口（问题 3 答案）

### 3.1 调用链全貌

```
QML: System.doRun_debug_script(N)                       ← com.hasselblad.proxies System 单例
  (StateMachineViewModel.qml L428-439; SystemProxyDbus::doRun_debug_script(int) @0x1067dc0 W 符号;
   DBus 槽 SystemProxyDbus::run_debug_script(int,int,QObject*) @0x1092ef8; P1 mod_main6.qml 亦实证
   System.run_debug_script(5) 形式可调用)
 → DBus com.hasselblad.system → camera-system SystemObjectImpl::doRun_debug_script
 → ScriptHandler::runScript(int) (@camera-system 0xfce28)
 → kScriptPathTemplate = **"/system/bin/custom_debug_%1.sh"**   ← camera-system 二进制 UTF-16 实证
    (相邻字符串 "Start debug script:"/"Invalid script id:"/"Script already running:" 等)
```

### 3.2 原生 UI 入口 = 自定义键下拉（零注入、官方支持）

- **QML 内唯一调用点**: `viewmodels/StateMachineViewModel.qml` handleCustomButton():
  `case E_UserButtonFunction_DebugScript1..4: System.doRun_debug_script(1..4)`。
- **下拉里有这个选项**: E_UserButtonFunction 值表标签 "Debug Script 1"–"Debug Script 4"
  (UTF-16 @0x1d16590/0x1d165ae/0x1d165cc/0x1d165ea，紧邻 "Live View AF Dump"/"Screenshot" 等自定义键功能标签)。
  菜单表 user_button_{custom_front,custom_rear,wheel_click_front,wheel_click_rear,joystick_click,m,iso_wb}_function
  (menux2d 0x3a5fac–0x3a8000, label "Front Custom Btn." 等)。
  → 原生路径: 设置→通用→Custom Buttons→给某键选 "Debug Script 1"→按该键 = 执行 /system/bin/custom_debug_1.sh。
- **注意**: 脚本由 **camera-system**(root) 进程执行，不是 GUI；/tmp/x2nes 要出画面需 weston/WAYLAND_DISPLAY 环境——脚本侧自行处理（screen 双缓冲/eagle-shell 的教训见 REPORT_P §6.2）。

### 3.3 隐藏工程/工厂菜单开关排查（结论：无一可点亮"入口"）

| 开关 | 性质 | 结论 |
|---|---|---|
| `GuiConfig.showSecretMenuItems` | 运行时易失 bool(默认 false)，唯一写口 = QML `GuiConfig.showSecretMenuItems = true`（gui_secret_menu.md；menux2d 中被它门控的项 = Service 菜单 "Additional Functions" 段的 FW Update Retry/Factory Reset/Clear Sensor Calibration/Grip FW Retry 按钮 + Demo Mode 段头） | **没有游戏/脚本入口**；可作 GAME 面板里的彩蛋开关 |
| `System.debug_options` (E_DebugOption 27位) | DBus 可写位掩码 | 只点亮 AF/HDR/Histogram 等 HUD 叠层（O3 §3.1），不改菜单结构 |
| E_MenuAvail_DeveloperCamera=8 | 条件枚举 | menux2d **0 项**使用（仅 CFV 后背）→ 机身无工程菜单可解锁 |
| E_MenuRW=32 NotImageProcessing | serviceMenu 的 rw 条件（0x396864 w4=0x20） | 非隐藏开关，仅处理图片时禁用 |
| persist.*/hbl.* 形式工程开关 | — | camera-gui 内无此类属性驱动菜单可见性的证据 |
| developerMenu/serviceMenu | 通用(General)下嵌套子菜单 | 常规可达（服务=传感器校准等），developerMenu 内是 E_DebugOption 开关列表 |

**没有比自定义键更原生的 run_debug_script 入口**；设置菜单里没有脚本执行类条目。

---

## 4. 注入方案评估（问题 4 答案）

### 方案 A: 全量 qrc 镜像 + appPrefix 前缀替换（像素级原生）

做法：modqml.so 在现有 BSS 补丁（0x2b054b8 → file:///tmp/modqml/m + mmain.qml，P1 v4 已验证）之上，
**再补 CommonConstants 副本 `0x2b494e8` 的 {ptr,size} 指向 RW 零填缓冲 "file:///tmp/modqml/"（20 字符等长）**。
效果：QML 里所有 `Constants.appPrefix + "..."` 构造的 URL（ControlDrawer→MainScreen.qml、SettingsGeneric、各 popover）全部改从磁盘加载；镜像 `core/qrc_dump/t021600e8/qt/qml/app/qml/` 整树(346文件)到 /tmp/modqml/，编辑 MainScreen.qml 加 GAME 格。

- 优点：GAME 成为真网格项（同一 delegate、翻译、焦点/摇杆导航全兼容）；未镜像/未编辑文件继续走 qrc+AOT 不受影响（硬编码 qrc: URL 的引用仍命中原资源，混用安全）。
- 工作量：★★（镜像=纯拷贝 3MB；补丁=modqml.so 加 2 行；改 1 个 QML）。
- 风险点：
  1. **QString::append 就地写陷阱**（P1 v3 实锤）：app_main 副本被 append("main.qml")——必须用 v4 式 RW 零填缓冲；CommonConstants 副本只读拼接(+)，风险低，但同样给 RW 缓冲最稳。
  2. svgimageprovider 副本 0x2b05b10 是否用 appPrefix 拼 image://svg 路径未证——若图标失效，同法补丁 + 镜像 icons 树（dump 已备）。另见 0x1cee566 独立串 "qrc:///icons/"，图标大概率走独立前缀不受影响。
  3. file: 加载的文件失去 AOT → 运行时编译（P1 证明可行；迭代期加 QML_DISABLE_DISK_CACHE=1 + rm /data/qmlcache）。
  4. 每次重启 GUI 需 ctl.stop/start weston（eagle-shell 输出不回收，REPORT_P §6.2）。
- 前置已备齐：qrc_dump 全树 + manifest。

### 方案 B: 原生自定义键 + custom_debug_N.sh（零注入）

做法：不写一行 QML。设置→通用→Custom Buttons→"Rear Custom Btn."= "Debug Script 1"；放 /system/bin/custom_debug_1.sh（内容 exec /tmp/x2nes 或启动包装）。
- 优点：★ 工作量、零修改、官方钩子、稳定。
- 缺点：**没有"GAME"菜单条目**（按键触发，非左滑菜单可见项）；/system 可能只读（脚本落地需 remount 或借 /tmp+软链方案——若 custom_debug_1.sh 必须在 /system/bin 则要看分区状态；P 报告未验证脚本放置）。
- 定位：作为兜底/调试通道，以及 GAME 条目点击后的实际执行后端（两方案共用 3.1 链路）。

### 方案 C（推荐）: 现有薄壳 + 运行时叠加 GAME 格 + doRun_debug_script

做法：沿用 P1/P2 已验证通道（modqml.so v4 + /tmp/modqml/mmain.qml 薄壳，其余全 qrc+AOT）。壳内 Timer 延迟遍历 objectName 找到 `MainScreen_root` / `MainScreen_grid`，在其页面上叠加一个 GAME 按钮：
- **C1（首选，~30 行）**：`Qt.createComponent(qrc:FramedImage 同款外观)` createObject 挂到 MainScreen 页；网格 4×3=12 格只占 11 格（11 分类实证），把 GAME 放在**空着的第 12 格**（几何可从相邻 delegate item 或 Constants 计算）——视觉上就是原生第 12 个分类。点击 → `System.doRun_debug_script(N)`。
- **C2（真模型行，可选进阶）**：重赋 `grid.model`（QML 属性绑定可命令式覆盖）：用 ListModel 复制 FavoriteModel 行 + GAME 行（roles: menuName/labelContext/label/iconSrc/itemEnabled）。GAME 行点击走 delegate 的 `loadSubMenuItem("GAME")` signal：壳里 connect 同名 signal 先行处理（launch），原 ControlDrawer 槽也会触发但 `SubMenu.menuFile("GAME")` map miss → 返回空串（0x1706d9c 实证）→ loader "Ignore show loader without source"，**无害**（menuRw miss 行为需真机确认，最坏弹一次 blocked toast）。
- 优点：★☆ 工作量（全部基于 P2 已验证的注入手法）、零二进制新增改动、秒级热迭代、原厂树零改动、回退=撤壳。
- 风险点：MainScreen 是 ControlDrawer 的 Loader 异步加载（active 由左滑触发）——注入时机要等首次左滑（或先 setMainState 预热）；EVF 模式下 touch 关闭（native 行为一致，无碍）；GAME 无专属图标→用现成 image://svg/ic_general_* 或 emoji 文本。

### 对比表

| 维度 | A 全量镜像 | B 自定义键 | C 薄壳叠加（推荐） |
|---|---|---|---|
| GAME 出现在左滑菜单 | ✅ 原生网格项 | ❌（物理键） | ✅ 第 12 格叠加（C2 可成真模型行） |
| 二进制/补丁改动 | modqml.so +1 处 BSS | 无 | 无（沿用现有 so） |
| QML 工作量 | 镜像+改 MainScreen | 0 | ~30-80 行壳内 JS |
| 热迭代 | 改 /tmp 文件 | 改脚本 | 改 /tmp 文件 |
| 新风险 | appPrefix 三副本副作用/图标 | 分区只读 | 注入时机/空格定位 |
| 执行后端 | 三者同一：doRun_debug_script(N) → custom_debug_N.sh | | |

### 推荐路线

1. **C1 落地**（1 次真机迭代）：薄壳加 GAME 格 + `System.doRun_debug_script(1)`；脚本 /system/bin/custom_debug_1.sh（或可用的编号）里 exec /tmp/x2nes（注意 weston 环境、重启 GUI 的 weston 回收序列）。
2. 观感不满足/需摇杆导航焦点 → 升级 **C2**（模型行）或 **A**（全镜像改 MainScreen.qml，物料已备）。
3. **B** 始终作为无壳兜底（键即游戏）。
4. 彩蛋：GAME 面板里可加 `GuiConfig.showSecretMenuItems = true` 开关（原生 secret 项即现）。

---

## 5. 本次新增资产

| 文件 | 说明 |
|---|---|
| `core/qrc_dump.py` | 全量 rcc 提取器（自包含，含格式修正注释） |
| `core/qrc_dump/` | 72 树 1112 文件 + manifest.json（主树 t021600e8=346 QML；设置菜单相关在 qt/qml/app/qml/mainmenu/ + controlscreen/ControlDrawer.qml + viewmodels/StateMachineViewModel.qml） |
| `core/MENU_INJECT_NOTES.md` | 本报告 |

## 6. 关键地址速查（fw v1.3.16.2, camera-gui 偏移）

| 项 | 偏移 |
|---|---|
| MenuX2d::topMenus / menuFile / menuLabel | 0x1706b40 / 0x1706ca8 (miss→空串 @0x1706d9c) / 0x1706b60 |
| Menu::conditionValid(E_MenuRW/E_MenuAvail) | 0x16f4ba8 / 0x16f4bb0 |
| Submenu::Submenu(QString,E_MenuOption,bool,E_MenuRW,E_MenuAvail) | 0x1703b08 |
| menux2d 顶层 Submenu 构造序列 | 0x395724–0x396c34（serviceMenu@0x396868 rw=NotImageProcessing(32); developerMenu@0x396c34 bool=1） |
| FavoriteModel::init/reloadModel/addItem | 0x16f1f80 / 0x16f1f48 / 0x16f2530（单例注册 0x16efb60） |
| SystemProxyDbus::doRun_debug_script(int) / run_debug_script(int,int,QObject*) | 0x1067dc0 / 0x1092ef8 |
| "Debug Script 1..4" 值表标签 (UTF-16) | 0x1d16590 / 0x1d165ae / 0x1d165cc / 0x1d165ea |
| 11 分类 menuName+icon 字面量区 (UTF-16) | 0x1cedb00–0x1cee566（qrc:/qt/qml/app/qml/ 前缀本体 0x1cedb5c） |
| camera-system kScriptPathTemplate "/system/bin/custom_debug_%1.sh" | camera-system 文件偏移 0x446290 (UTF-16) |
| kAppResourcePrefix 三副本 BSS | 0x2b054b8(app_main,P1 已补丁) / 0x2b05b10(svgimageprovider) / 0x2b494e8(CommonConstants, 方案 A 目标) |
