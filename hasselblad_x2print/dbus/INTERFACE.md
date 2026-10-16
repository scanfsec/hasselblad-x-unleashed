# com.hasselblad.printer — 通用 IPP 打印服务 D-Bus 接口规格书（v2.3）

日期:2026-09-23 | 本文件是**唯一契约**：GUI 重建任务、printerctl、printer-service 共同遵守。
线协议逐条对齐 `analysis/dbus/REPORT_odindb_conventions.md`（odindb/AppsMessaging 逆向结论）。

> **v2.3 反饿死与恢复完备性（P0/P1，NOTES §22）**：
> 1. `Start_discovery` 语义改为**受理即返**（§3）：返回空 as，结果经 `discovered_printers`
>    notify 逐项发布，完成=connection_state 离开 discovering；总预算 60s。
> 2. `Disconnect_and_restore` 受理即返（§4b-1）：恢复序列后台推进，完成经
>    propertiesChanged 广播；对客户端语义不变（幂等且完备）。
> 3. `printer_ip` 键定性为纯内部会话缓存，会话恢复时无条件清除（§5）。

> **v2.2 SSID 扫描选择（§3 新增 `Scan_wifi` + §2 新增属性 `scanned_networks`）**：
> 1. 新方法 `Scan_wifi() → as`：Wi-Fi 站点扫描（阻塞至完成，真机约 2–4s；mock 2~2.5s（mock 两处联调桩略异，真机 iw 路径 ~3s））。
>    返回列表元素 `"SSID\tRSSI(dBm)\tSEC"`（tab 分隔；SEC=`wpa2`|`open`）。
>    隐藏网络（空 SSID）跳过；同 SSID 去重取最强；按 RSSI 降序；≤32 项。
> 2. 新属性 `scanned_networks`（`as`，只读，notify）：最近一次 Scan_wifi 的结果，
>    扫描完成时发布——GUI 列表直接绑定它（属性与方法返回同源）。
> 3. **时序（连接流程升级，Connect_to_ap 签名不变）**：`Scan_wifi`（阻塞返回/
>    `scanned_networks` notify）→ GUI 滚动列表高亮选择（选中即得 SSID/SEC）→
>    仅 WPA2 网络需要密码（open 网络免密直连）→ `Connect_to_ap(ssid, psk)`
>    （open 网络 psk 传**空串**：服务端空 psk=开放网络，wpa.conf 落 `key_mgmt=NONE`；
>    1..7 位半截密码仍 `failed`）。
>    扫描仅 `disconnected` 态可用：joining/joined/ready/discovering/扫描中 → `busy`。
> 4. 芯片语义（相机）：扫描前置与 Connect_to_ap 相同（wlan0 缺失则 insmod+up；
>    原生 AP 在跑先 `power_on_wifi(false)` 合法让出并记忆原状）；**扫描后保持芯片
>    可用不拆除**（用户即将连接，重扫/连接都快路径）——若放弃，由闲置看门狗
>    （§4b-3 已扩展到 disconnected+扫描保留态）或 Disconnect_and_restore 收尾。
>    会话防护（§4b-2b）自扫描受理起持有。执行层优先 `iw dev wlan0 scan`，
>    iw 不可用时回退自带 wpa_supplicant + `wpa_cli scan/scan_results`（解析双格式）。

> **v2.1 自由裁切/旋转/预览（§3 新增 3 方法 + §3b 编辑参数契约）**：
> 1. 新方法 `Get_photo_preview(s,i,i)→s`（快速预览图，带缓存）、`Get_photo_info(s)→a{sv}`
>    （头部探测）、`Preview_print(s,a{sv})→s`（WYSIWYG——与打印完全同管线同参数）。
> 2. `Start_print_job` / `Preview_print` 的 opts 扩展编辑键：`crop_x/crop_y/crop_w/crop_h`（d，
>    归一化 0..1，定义在**旋转后**的图上）与 `rotation`（i，-1=EXIF 自动 / 0/90/180/270 显式覆盖）。
> 3. 管线顺序定死（§3b 坐标系定义）：解码 → 旋转 → 裁切 → 纸型比例适配 → HQ 缩放 → 锐化 → 编码。
>    **`doPreview_print` 的输出与 `Start_print_job`（同参数）实际发出的 JPEG 逐字节一致**。
>
> **v2 通用化重构（破坏性变更，GUI 任务尚未消费，趁窗口期定稿）**：
> 1. 删除固定枚举属性 `paper_size`/`media_type`，`color_mode`/`printer_quality` 的 int 语义废除，
>    改为**打印机原生 IPP 关键字字符串**属性 `media`/`print_quality`/`color_mode`。
> 2. 新增能力属性（`Select_printer` 成功即探测 Get-Printer-Attributes[all+media-col-database]
>    后 notify）：`supported_media`/`supported_qualities`/`supported_color_modes`/
>    `supported_formats`/`copies_max`/`caps_ready`/`media_size_info`/`document_format`。
> 3. Set 校验以能力为源：`caps_ready` 后 media/print_quality/color_mode 必须命中
>    supported_*，否则 `parametererror`；`copies ≤ copies_max`。
> 4. `Start_print_job` 的 opts `a{sv}` 键=IPP 关键字（copies/media/print-quality/
>    print-color-mode/print-scaling），透传前按能力校验（§3）。
> 5. 打印目标尺寸自计算：选中 media 物理尺寸（media-col µm）× printer-resolution-default
>    （缺失兜底 300dpi）= imgprep 目标像素；一切打印机属性自探测，零硬编码。

---

## 1. 总线与标识

| 项 | 值 |
|---|---|
| 总线 | system bus，相机 socket `unix:path=/dev/socket/dbus`（用 `DBUS_SYSTEM_BUS_ADDRESS` 注入；WSL 联调用 session bus） |
| bus name (destination) | `com.hasselblad.printer` |
| object path | `/printer` |
| 业务接口名 | `com.hasselblad.printer`（= bus name，odindb 同体约定） |
| 属性接口 | `org.freedesktop.DBus.Properties` 的 `Get(s)→v`、`Set(ssv)`、`GetAll(s)→a{sv}`；**Get/Set/GetAll 的 interface 实参必须填 `com.hasselblad.printer`** |
| 属性变更信号 | `org.freedesktop.DBus.Properties.propertiesChanged`（**首字母小写 p！非标准拼写**），path=`/printer`，签名 `sa{sv}as`，arg2（invalidated）恒为空数组 |
| 方法 member | odindb 名去掉 `do` 前缀，**snake_case 原样**（如 `Connect_to_ap`） |
| 枚举封送 | i32（`i`）；v2 起打印机原生选项一律字符串（`s`），仅 copies/preset 仍是 `i` |
| 错误名 | `com.hasselblad.error.{failed\|parametererror\|busy\|canceled\|timeout}`（全小写连写）；`failed` 正文可带后端详情（如格式协商失败原因） |
| Introspect | 实现 `org.freedesktop.DBus.Introspectable.Introspect` → 手工 XML（见 §6） |

GUI 侧等效 match rule（QtDBus 生成式）：
```
type='signal',sender='com.hasselblad.printer',path='/printer',interface='org.freedesktop.DBus.Properties',member='propertiesChanged'
```

---

## 2. 属性全表（v2.2：24 个，全部 notify；名称一律小写 snake_case）

GetAll 返回顺序即下表顺序。`W` 列=可 Set；其余只读，Set 只读项回 `parametererror`。

| # | 属性 | D-Bus 签名 | W | 初始值 | 取值 / 含义 |
|--:|---|---|---|---|---|
| 1 | `connection_state` | `s` | – | `"disconnected"` | `disconnected` \| `joining` \| `joined` \| `discovering` \| `ready`（§4 状态机） |
| 2 | `ssid` | `s` | ✓ | `""` | 目标打印机热点 SSID（可预置到 config.ini） |
| 3 | `printer_state` | `s` | – | `"idle"` | `idle` \| `busy` \| `error`（打印机自身状态，探测回填） |
| 4 | `printer_name` | `s` | – | `""` | 当前选中打印机名（Select_printer 成功后非空） |
| 5 | `printer_ip` | `s` | – | `""` | 打印机 IP（选中后填入） |
| 6 | `job_progress` | `i` | – | `0` | 0–100 百分比，随 job_status 推进 |
| 7 | `job_status` | `s` | – | `"idle"` | `idle` \| `preparing` \| `sending` \| `printing` \| `done` \| `failed` \| `canceled`（§4） |
| 8 | `copies` | `i` | ✓ | `1` | 打印份数 1–copies_max，越界 → `parametererror` |
| 9 | `media` | `s` | ✓ | `""` | **打印机原生 media 关键字**（如 `na_index-4x6_4x6in`）；探测后=打印机 media-default；caps_ready 后必须命中 `supported_media` |
| 10 | `print_quality` | `s` | ✓ | `"normal"` | `draft` \| `normal` \| `high`（RFC 8011 print-quality 3/4/5 的关键字名）；探测后=打印机默认并校验命中 |
| 11 | `color_mode` | `s` | ✓ | `"auto"` | **原生 print-color-mode 关键字**（`color`/`monochrome`/`auto`/`auto-monochrome`…以探测为准）；探测后=打印机默认并校验命中 |
| 12 | `preset` | `i` | ✓ | `0` | 应用层预设：0=默认 1=证件照 2=拍立得（与打印机能力无关，不下发） |
| 13 | `document_format` | `s` | – | `""` | 实际下发的 document-format（选中后=`image/jpeg`；格式协商见 §2b） |
| 14 | `caps_ready` | `b` | – | `false` | 能力探测完成（Select_printer 成功即 true；断开/重选失败回 false 并清空能力属性） |
| 15 | `copies_max` | `i` | – | `99` | 该打印机 copies-supported 上限（未探测=99） |
| 16 | `supported_media` | `as` | – | `[]` | 可下发 media 关键字列表（media-supported + media-col 提炼） |
| 17 | `supported_qualities` | `as` | – | `[]` | 该打印机支持的 print-quality 关键字子集（如仅 `["normal","high"]`） |
| 18 | `supported_color_modes` | `as` | – | `[]` | 原生 print-color-mode-supported 列表 |
| 19 | `supported_formats` | `as` | – | `[]` | document-format-supported 列表 |
| 20 | `media_size_info` | `as` | – | `[]` | 每项 `"keyword=WxHmm"`（如 `na_index-4x6_4x6in=101.6x152.4mm`），人机两读 |
| 21 | `discovered_printers` | `as` | – | `[]` | 最近一次 Start_discovery 的结果（元素格式 `"名字\tIP"`，tab 分隔） |
| 22 | `scanned_networks` | `as` | – | `[]` | **v2.2** 最近一次 Scan_wifi 的结果（元素 `"SSID\tRSSI(dBm)\tSEC"`，SEC=`wpa2`\|`open`；RSSI 降序 ≤32 项；扫描完成 notify） |
| 23 | `synced` | `b` | – | `true` | odindb 兼容（§7b） |
| 24 | `tx_active` | `b` | – | `false` | odindb 兼容（§7b），tx() 执行期间 true |

Set 线格式：`Properties.Set("com.hasselblad.printer", name, variant)`，签名不匹配回 `org.freedesktop.DBus.Error.InvalidArgs`，语义错误（只读/越能力/未知名/空字符串）回 `com.hasselblad.error.parametererror`。Set 成功回空回复并随即广播 `propertiesChanged`（单属性 dict）。

## 2b. 能力探测与协商（v2：一切之源，零硬编码打印机属性）

`Select_printer` 成功后服务立即执行 Get-Printer-Attributes（requested = `all` +
`media-col-database`，集合摊平解析），随后：

1. **格式协商**（决定 `document_format`）：
   - `image/jpeg` ∈ `document-format-supported` → 直发 JPEG，`document_format="image/jpeg"`；
   - 否则若仅栅格格式（`image/pwg-raster`/`image/urf`/`cups-raster`）→ Select_printer 回
     `failed`，正文「该打印机需栅格后端，暂不支持」；
   - 否则回 `failed`（格式列表不含 image/jpeg）。
2. **能力发布**：`supported_media`（仅原生关键字，media-col 合成显示名不进）、
   `supported_qualities`（枚举 3/4/5 → draft/normal/high 关键字）、`supported_color_modes`、
   `supported_formats`、`copies_max`、`media_size_info`（仅通告尺寸的 media）逐属性 notify；
   `caps_ready=true`。
3. **默认值归一**：`media`=打印机 media-default、`print_quality`=print-quality-default、
   `color_mode`=print-color-mode-default（均校验命中能力列表，未通告取列表首项/normal）；
   `copies` 超上限自动钳制并 notify。
4. **尺寸自计算**（Start_print_job 时）：选中 media 的物理尺寸（media-col x/y-dimension µm，
   见 `media_size_info`）× `printer-resolution-default`（缺失兜底 300dpi）= 目标像素；
   内容（EXIF 旋转后）横竖与纸张横竖不一致时交换宽高；方纸不交换。
5. **Set 双闸**：`caps_ready` 后 `media`/`print_quality`/`color_mode` Set 值必须命中对应
   supported_*（否则 `parametererror`）；`Start_print_job` opts 覆盖值同样按能力校验。

发现侧（v2）：mDNS 同时查 `_ipp._tcp.local` 与 `_universal._sub._ipp._tcp.local`
（AirPrint 标记），TXT `rp=` 路径非 `/ipp/print` 也支持（端点路径/端口挂服务内部侧表，
`discovered_printers` 线格式不变）；直连兜底 `printer_ip` 支持**逗号分隔多端点
`host[:port]`**（多台打印机彩排）。

---

## 3. 方法全表（member = 名字去掉 do 前缀；返回多值平铺，不包 struct）

| C 函数 | wire member | 入参签名 | 返回签名 | 语义 | 错误 |
|---|---|---|---|---|---|
| `doConnect_to_ap` | `Connect_to_ap` | `ss` (ssid, psk) | `b` | 加入打印机热点（WPA2-PSK）。立即返回=是否受理（TRUE 后异步走 joining→joined，失败回 disconnected 并发信号）。相机执行层（wifi_backend=cam）＝NOTES §15 实测序列：原生 AP 在跑则先 `power_on_wifi(false)` 合法让出（记忆原状）→ insmod bcmdhd（仅当 wlan0 缺失，取所有权）→ ifconfig up → 自带 wpa_supplicant(-Dnl80211) → 轮询 wpa_state=COMPLETED(45s) → udhcpc。**psk 语义**：仅经本方法入参传入，落 `/data/x2print/psk` 与 `/data/x2print/wpa.conf`（均 chmod 0600）；绝不做成 D-Bus 属性（无任何 psk 读接口）、不进命令行/进程表、不进日志 | `busy`（已在 joining/joined/ready）、`parametererror`（空 ssid）、`failed`（psk 格式即错，1..7 位半截密码；v2.2 起**空串=开放网络**免密直连，wpa.conf 落 key_mgmt=NONE；相机：wpa 二进制缺失/参数非法） |
| `doScan_wifi` | `Scan_wifi` | `` | `as` | **§20/v2.2 SSID 扫描选择**：Wi-Fi 站点扫描，阻塞至完成（真机 `iw dev wlan0 scan`，iw 缺失回退自带 wpa_supplicant+`wpa_cli scan/scan_results`；mock 2.5s 返回 4 假网络）。返回元素 `"SSID\tRSSI(dBm)\tSEC"`（SEC=`wpa2`\|`open`；隐藏网络跳过、同 SSID 去重取最强、RSSI 降序、≤32 项），结果同步写入 `scanned_networks` 并 notify（GUI 列表绑定它）。相机芯片语义：前置与 Connect_to_ap 相同（wlan0 缺失则 insmod+up；原生 AP 在跑先合法让出+记忆）；**扫描后保持芯片可用不拆除**（用户即将连接；放弃由闲置看门狗/Disconnect_and_restore 收尾）；会话防护（§4b-2b）自扫描受理起持有。P0：真后端受理后在后台 tick 机推进（芯片让出/保障/扫描全程子进程化），方法回复由服务挂起至完成再补发——**客户端可见契约不变**（阻塞至完成），期间服务照常应答其它调用 | `busy`（joining/joined/ready/discovering/扫描中）、`failed`（芯片/二进制不可用/被断开中断） |
| `doDisconnect_and_restore` | `Disconnect_and_restore` | `` | `` | **幂等且完备**（§4b 恢复协议）：任何 connection_state（含 joining 中途）、重复调用均收敛到 disconnected + 相机 Wi-Fi 基线。相机序列：取消活动任务 → wpa_cli terminate → ip addr flush → ifconfig down → rmmod bcmdhd（仅当本会话 insmod 过，扫描 insmod 的同样归此收尾）→ **合法电源同步** `power_on_wifi(原状)`（会话/扫描开始原生 AP 在跑→true 回用户原状；否则 false 合法关，不让 camera-system 状态机与实际失同步）→ 清空 printer_name/ip/discovered/caps | `failed`（后端失败） |
| `doStart_discovery` | `Start_discovery` | `` | `as` | **受理即返**（v2.3，与 `Connect_to_ap` 同语义；P0 反饿死）：受理返回**空 as**，`busy`/`failed` 同步错误。**四级级联在后台 tick 状态机推进**（§2b/§16）：L1 config `printer_ip` 缓存端点（逗号分隔 `host[:port]`，上次发现回写）直连 → L2 mDNS（`_ipp._tcp`+`_universal._sub` AirPrint 子类型，TXT `rp=` 端点路径）→ L3 默认路由网关 → L4 wlan0 所在 /24 TCP:631 快扫+IPP 验明。每端点/窗口为独立子进程（黑洞端点 8s 预算快速放弃，**总预算 60s** 到点即返已发现结果）。结果**逐项经 `discovered_printers` notify** 发布（GUI 属性驱动），完成=`connection_state` 离开 `discovering`；期间服务照常应答 Get/GetAll（≥1 次呼吸/2s）。命中端点回写 config `printer_ip` | `busy`（joining/discovering 中）、`failed`（未连接） |
| `doSelect_printer` | `Select_printer` | `s` (name) | `b` | 从发现列表选中打印机 → 填 printer_name/printer_ip，connection_state→ready，**并立即能力探测**（§2b：格式协商+能力属性发布+默认值归一，逐属性 propertiesChanged）。name 接受元素整串（`名字\tIP`）、纯名字或纯 IP | `parametererror`（不在 discovered 列表）、`failed`（未发现过/探测失败/需栅格后端） |
| `doGet_photo_list` | `Get_photo_list` | `s` (path) | `as` | 列目录下 jpg/jpeg（≤64 项）。path 是相机相册目录 | `parametererror`（空路径/目录不存在） |
| `doStart_print_job` | `Start_print_job` | `as` (paths), `a{sv}` (opts) | `s` (job_id) | 提交打印任务。**opts 键=IPP 关键字**（v2）：`copies(i)`、`media(s)`、`print-quality(s)`、`print-color-mode(s)`、`print-scaling(s)`；**§18 编辑键**：`crop_x/crop_y/crop_w/crop_h(d)`、`rotation(i)`（语义/校验见 §3b）；未提供=用同名属性当前值（编辑键默认全幅+EXIF 自动；注意 media/print-quality/print-color-mode 用连字符，属性名用下划线）；未知键忽略。透传前按能力校验（越能力值→parametererror）。job_id 形如 `job-<n>`。仅 ready 态且无进行中任务 | `busy`（任务进行中）、`parametererror`（paths 空/opts 值越能力或编辑键非法）、`failed`（未 ready/能力未就绪） |
| `doCancel_job` | `Cancel_job` | `` | `b` | 取消当前任务：TRUE=已取消（本地立即 job_status→canceled→idle；IPP Cancel-Job(0x0008) 下发+≤3s 有界确认环在服务后台子进程执行，结果经 rx 事件帧 ev=2 与日志可见——方法即返不阻塞），FALSE=无进行中任务（不是错误，原因见服务日志） | — |
| `doGet_photo_preview` | `Get_photo_preview` | `s` (path), `i` (max_w), `i` (max_h) | `s` (jpeg_path) | **§18 快速预览图**（照片网格/编辑页底图）：FAST 管线（无锐化），EXIF 自动旋转，长边 ≤ max（max_w/max_h ≤0 或缺省=1024，上限 4096），保宽高比缩小（不放大）。3FR 感知=内嵌 preview 提取（fseek/fread 命中区段）。**带缓存**：`/data/x2print/preview/<路径哈希>_<w>x<h>.jpg`（WSL=`/tmp/x2print/preview/`），源文件 mtime 变更自动失效；返回**绝对路径**，GUI 直接按文件加载 | `parametererror`（路径空/文件不存在）、`failed`（解码失败/3FR 无 preview） |
| `doGet_photo_info` | `Get_photo_info` | `s` (path) | `a{sv}` | **§18 头部探测**（不解码像素）：`width(i)`、`height(i)`（存储尺寸）、`orientation(i)`（EXIF 1..8，0=无）、`rotated_width/rotated_height(i)`（EXIF 折算后逻辑尺寸）、`has_jpeg(b)`（文件本身是 JPEG）、`is_3fr(b)`（哈苏 RAW，尺寸/orientation 基于内嵌 preview）、`file_size(x, i64)`。GUI 显式旋转时自行交换 rotated 宽高 | `parametererror`（路径空/文件不存在）、`failed`（非图像/3FR 无 preview） |
| `doPreview_print` | `Preview_print` | `s` (path), `a{sv}` (opts) | `s` (jpeg_path) | **§18 WYSIWYG 打印预览**：与 `Start_print_job` **完全同管线同参数**（同 crop/rotation/media/print-scaling/HQ/锐化/编码；`backend rj_prep_file` 共用）——**输出与打印实际发出的 JPEG 逐字节一致**。opts 键同 Start_print_job（`copies` 忽略），能力校验同闸（media 须命中 supported_media）。输出落 `/data/x2print/preview/job_<n>_preview.jpg`（n 服务级递增，环形保留最近 8 份），JPEG 含 300dpi 密度元数据（GUI 可从文件读打印尺寸） | `busy`（打印任务进行中）、`parametererror`（路径空/opts 越能力/编辑键非法含裁切窗过小）、`failed`（未选打印机/能力未就绪/图像处理失败） |

odindb 客户端归类：5 类 `com.hasselblad.error.*` 按名比对，message 为人读文本。

## 3b. 编辑参数契约（§18 裁切/旋转，GUI 编辑页照此实现）

**管线顺序（定死，imgprep 实现）**：

```
解码(DCT 缩放档) → 旋转 → 裁切(用户归一化矩形) → 纸型比例居中适配
      → HQ 缩放(Lanczos3) → 锐化(unsharp) → 编码(q95 4:4:4 300dpi)
```

**坐标系定义（关键）**：`crop_x/crop_y/crop_w/crop_h` 归一化 0..1，定义在
**旋转后**的图上——即 GUI 预览里用户看到的那幅画面（`Get_photo_preview` 的
EXIF 自动旋转结果 / 显式 rotation 应用后的画面）。x/y 为左上角原点，y 向下。

**rotation 语义**：`-1` = EXIF Orientation 自动（现行为，默认）；
`0/90/180/270` = 显式顺时针旋转，**覆盖** EXIF（不叠加）——即旋转以存储
像素为基准重算，EXIF 标记被忽略。GUI 的旋转按钮：初值=-1（跟随 EXIF），
用户点 90/180/270 后发对应值。

**校验（adaptor 层即拒，`parametererror`）**：
- crop 四值必须 0..1 且 `x+w≤1`、`y+h≤1`（1e-6 容差）、`w>0`、`h>0`，NaN/Inf 拒；
- rotation 必须 `-1/0/90/180/270`；
- 裁切窗最小尺寸：两维源像素 ≥100px（`IMGPREP_MIN_CROP_PX`，按旋转后逻辑
  尺寸算；`Preview_print` 预检，`Start_print_job` 在 prep 阶段失败→job failed）；
- 越界钳制（合法但 x+w 略超等）：服务端钳到边界内（`w=1-x`），日志可见钳后窗。

**裁切后的纸型适配**：裁切窗按目标纸型宽高比**居中裁切**（cover/填充语义，
与无裁切时的行为一致）；GUI 画预览框时按同规则给用户视觉反馈（窗内再裁
居中带）。方纸不裁。

**WYSIWYG 保证**：`Preview_print(path, opts)` 输出 == `Start_print_job([path],
opts)` 实际发给打印机的 JPEG（同管线、同参数、确定性编码器），集成测试
`test_integration_wsl.sh (i10g)` 逐字节断言。GUI 只需保证预览与提交用同一
份 opts。

**printerctl 示例**（crop_* 小数自动封 double，rotation/copies 整数封 i32）：
```
printerctl call Get_photo_preview /mnt/.../B0000007.3FR 1024 1024
printerctl call Get_photo_info    /mnt/.../B0000007.3FR
printerctl call Preview_print     /mnt/.../B0000007.3FR crop_x=0.5 crop_y=0 crop_w=0.5 crop_h=1 rotation=90
printerctl call Start_print_job   /mnt/.../B0000007.3FR crop_x=0.5 crop_y=0 crop_w=0.5 crop_h=1 rotation=90
```

---

## 4. 状态机

```
connection_state:
  disconnected ──Connect_to_ap(ss)──▶ joining ──关联+DHCP 成功──▶ joined
       ▲                                │失败(信号可见)            │
       │                                ▼                         │Start_discovery()
       │                           disconnected        discovering
       │                                                          │扫描完成(结果→discovered_printers)
       │                            ready ◀──Select_printer(s)────┘
       └────────────────Disconnect_and_restore()（任何态均可）─────┘
                                   （同时取消任务、清 printer_name/ip）

job_status（与 connection_state 正交，仅在 ready 态有意义）:
  idle ──Start_print_job──▶ preparing ─▶ sending ─▶ printing ─▶ done ─(2s)─▶ idle
                 │              │           │           │
                 │        Cancel_job: canceled ─(1s)─▶ idle
                 └─────── 任一阶段失败 ──▶ failed ─(2s)─▶ idle
  job_progress: preparing 固定 0；sending 10→40（进入即 10，此后 10+30·k/n）；printing 40→100（mock 2%/秒）；
                done/failed/canceled 保持终值，回 idle 时清 0。
  printer_state: idle⇄busy（任务进行=busy），后端错误=error。
```

## 4b. 收尾恢复协议与闲置看门狗（v2 收官，硬要求）

打印完成或 GUI 关闭打印界面时必须**可靠恢复**；GUI 崩溃/忘了调也有兜底：

1. **GUI 主动关闭**：`Disconnect_and_restore()` 幂等且完备——重复调用安全、
   任何 connection_state（含 joining/扫描/发现中途）调用都收敛到 `disconnected` +
   相机 Wi-Fi 基线（零残留：无 wlan0 地址/无 supplicant/自载模块已卸）。
   **受理即返（P0）**：在途子进程（insmod/udhcpc/探测）立即 SIGKILL，恢复
   序列（wifi 反序→合法电源同步→防护释放→清场）后台推进，完成时
   `propertiesChanged`（connection_state→disconnected）广播可见；重复调用幂等受理。
2. **合法电源同步**（相机 wifi_backend=cam）：会话开始探测原生 AP 原状
   （hostapd 在跑或 wlan0=192.168.2.1 → 用户在用），在跑则先
   `com.hasselblad.system.power_on_wifi(false)` 合法让出并记忆；恢复末尾按
   记忆回原状（原开→`power_on_wifi(true)` 重新拉起 AP；原关→合法关）。
   绝不绕过状态机直接 rmmod 了事——否则 camera-system 的 wifi_power 内部
   状态与实际失同步（Phocus 蓝牙拉起热点失效的根因之一，NOTES §16.4）。
   仅首次恢复执行合法同步（重复 disconnect 不反复 restart AP）。
2b. **会话-待机防护**（NOTES §19，用户口径「打印的时候不息屏，打印完再
   恢复」）：Connect_to_ap 受理起至恢复完成，服务持双层防护——①屏幕层
   （主守护）：`screen_idle_timeout` 临时置 86400（该属性 Set 本身会唤醒
   屏幕；原值落盘 /data/x2print/screen_timeout.saved 供崩溃恢复），恢复完成
   在合法电源同步**之后**还原原值——息屏态下 camera-system 对
   power_on_wifi「受理但忽略」（`Wifi settings ignored when wifi is
   blocked`，§18.5「AP 烧掉」误诊的真相），屏亮才是电源请求被执行的先决
   条件；②内核带：wakelock `x2print-session`（/sys/power/wake_lock）挡
   autosleep 深度挂起（2h 档；不挡 FSM 待机也不挡直写 /sys/power/state）。
   服务启动 boot_init 清陈旧锁并按落盘凭据还原屏超时。
3. **闲置看门狗**：config `idle_restore_min`（缺省 5，0=禁用）。joined/ready
   且无打印任务且**无业务方法调用**（含 Properties.Set；Get/GetAll 纯轮询
   不算，GUI 开着不干活照样算闲置）超过该分钟数 → 自动执行与
   `Disconnect_and_restore` 相同的恢复路径，`propertiesChanged`
   （connection_state→disconnected）广播可见。打印任务结束视为最近活动
   （闲置时钟自任务完成重算）；join/discovering 进行中不触发；活跃操作
   （任务/阻塞扫描）期间由各自超时兜底（任务总超时 120s 等）。
   **v2.2 扩展**：`Scan_wifi` 后芯片保持可用而用户放弃连接（仍 disconnected、
   扫描保留态）同样计时，到点自动收尾（拆芯片/终止扫描 supplicant/还原电源
   原状）——「扫完不连」不会遗留 wlan0 up/自载模块/让出过的原生 AP。
4. **电源循环断言**：`service.sh verify`（部署于 /data/x2print/bin/x2_verify.sh，
   经 `printerctl syswifi on|off` 驱动）——`power_on_wifi(true)` → 轮询断言
   wlan0=192.168.2.1 + hostapd 在 + SSID 配置在案 → `power_on_wifi(false)` →
   断言 hostapd 停、wlan0 回 DOWN。相机侧 QtDBus 槽签名严格：恰一个 bool，
   不带 odindb wrapper 的 timeout 尾参（odindb-cli `-m` 带尾参会被告
   `signature 'bi'` 不匹配，故走 printerctl 专用通路）。§19 起 verify 跑前
   覆写 `screen_idle_timeout=86400`（保屏亮+唤醒屏，结束还原原值）+自持
   wakelock——息屏/待机态下断言仍恒成立；末尾 STA 痕迹为信息项（不断言）。

---

## 5. config.ini（/data/x2print/config.ini，手写最小 ini，无第三方依赖）

首启不存在时自动写入下述默认值；Set 可写属性成功后即落盘；SIGTERM 退出时再存一次。

```ini
; x2print printer-service 配置(自动生成, v2)
[printer]
ssid =
copies = 1
media =
print_quality = normal
color_mode = auto
preset = 0
```

- 解析规则：`;`/`#` 注释、`[节]`（节名仅注释性，键全局唯一）、`key = value` 首个 `=` 切分、两端去空白、CRLF 容忍。
- 未知键忽略（前向兼容）；非法值取默认并告警。
- `printer_ip` = **内部会话缓存**：发现学得后由服务自动回写（下次发现走 L1 直连快路径），**会话恢复（Disconnect_and_restore/看门狗/失败回滚）时无条件清除**——勿手改（会被清）。`idle_restore_min` 见 §4b。psk 不进 config.ini
  （单独存 `/data/x2print/psk`，0600）。

## 5b. 日志

`--log <file>` 覆盖，默认 `/data/x2print/log/service.log`（自动建目录；>256 KB 截断重写）；同时输出 stderr。级别 `--loglevel debug|info|warn|error`，默认 info。

---

## 6. Introspect XML（手工静态串；dbus-send/busctl 验证用）

要点：节点含 3 个接口声明（业务 + Properties + Introspectable + Peer）；propertiesChanged 信号声明在 **Properties** 接口里且 member 拼写小写。完整 XML 见 `src/printer-service/adaptor.c` 的 `INTROSPECTION_XML`（与本表一致，由 `test_wsl.sh` 第 4 项用 python3 xml 校验合法性）。

---

## 7. odindb 兼容性说明（互操作事实）

1. **现版 `odindb-send` 无法直接驱动本服务**：其 `ServiceFactory::createService` 硬编码 16 个模块名（gui/system/usbhost/storage/testd/camera/error/phocus/prodinfo/upgrade/exmcu/settings/tcphost/hosttransfer/wltcphost/suc），无 `printer`。互操作三选一：
   - (a) patch odindb-send factory 补一个名字（单点改动）——**已落地**：等长改名补丁副本 `tools/odindb-send-printer`（usbhost→printer），服务端对应兼容面见 §7b；
   - (b) `/system/bin/dbus-send`（dbus.conf 全放行）——test_wsl.sh 的验收方式；
   - (c) 本工程自带 `printerctl`（纯 libdbus 客户端，odindb-send 风格子集）——GUI 重建前的验收主力。
2. **验收替代法**：`printerctl monitor` 完整解析 `sa{sv}as` 并按 GUI（QtDBus 严格按槽签名匹配）视角解码打印——它读得通即证明 GUI 读得通。
3. 相机 GUI（camera-gui）当前无 PrinterProxyDbus 生成类，重建 GUI 时按本文件手写 QML/C++ 代理即可（线协议已兼容）。
4. 不占用既有 16 模块名（避免与真实服务冲突）；msg2dbus 30303 远程通道不含 printer handler（本机调用不受影响）。

## 7b. odindb-send-printer 兼容面（5 项）

补丁版工厂 CLI `odindb-send-printer`（`tools/patch_odindb_send.py` 产物，35 字节等长改名 usbhost→printer，详见 `analysis/dbus/REPORT_odindb_send_printer_patch.md`）驱动本服务走 **usbhost wrapper 的固定调用面**。服务端已补齐且**仅补齐**下列 5 项（CLI 的 moc 面决定它读不到/调不到其它任何属性方法）：

| 项 | 线格式 | 语义 |
|---|---|---|
| 方法 `tx` | `tx(y message_len, ay message)` → 空回复 | **原始调试字节管道**：收到即 LOGD 记录 hex（≤64 B 全打，更长截断标省略）并回显 Ok；同时把收到的字节**原样回发一帧 `rx`（echo 模式）**，供 CLI 自测环回。不下发后端，未来可扩展为 IPP/命令透传（载荷协议在 `ay` 内自定义） |
| 信号 `rx` | `rx(y, ay)`，path=/printer | 异步事件管道。两类触发：① 打印任务 `job_status` 每次变更（backend_real）发一帧 **X2P 事件帧**（`"X2P"` magic + u16 ev_type(1=job_status/2=cancel 结果) + u32 job_id + ASCII 摘要，字段大端，帧格式详见 `backend_real.c` rj_emit_rx 注释）；② `tx()` echo 回显（原样字节）。emit 函数 `prn_adaptor_emit_rx()`（adaptor.h） |
| 信号 `testrx` | `testrx(y, ay)` | 自测试预留，`prn_adaptor_emit_testrx()`（本期无内部触发方） |
| 属性 `synced` | `b`，只读，notify，初始 `true` | 兼容占位（本服务无链路同步概念，恒 true） |
| 属性 `tx_active` | `b`，只读，notify，初始 `false` | `tx()` 处理期间同步翻转 true→false（单线程，各发一次 `propertiesChanged`） |

- `synced`/`tx_active` 在 §2 属性表尾（GetAll 输出第 23/24 项），与其它属性同一 Properties 接口读写。
- **完整属性/方法访问请用 `printerctl` 或 `/system/bin/dbus-send`**——`odindb-send-printer` 的 `-p` 只认 wrapper moc 表已声明属性（即 synced/tx_active），`-m` 只有 `tx`。

设备端示例（`/data/x2print/odindb-send-printer`，见补丁报告 §5）：

```sh
B=/data/x2print/odindb-send-printer
$B -s -l                       # 列服务: printer 在列(usbhost 消失)
$B -s printer -p synced        # 属性读 → synced = true
$B -s printer -p tx_active     # → tx_active = false
$B -s printer -m tx 3 41,42,43 # tx(y=3, ay=41 42 43) → Ok;LOGD 记录 hex, 并 echo 一帧 rx(3, 41 42 43)
$B -s printer --signal rx      # 持续监听 rx(y,ay): tx echo 帧与打印任务 X2P 事件帧均有
$B -s printer --signal all     # tx_active 翻转的 propertiesChanged 可见
```

WSL 回归等价命令（不依赖补丁副本，test_wsl.sh 第 (6) 组）：`dbus-send --print-reply=literal --dest=com.hasselblad.printer /printer com.hasselblad.printer.tx byte:3 array:byte:65,66,67`；`Properties.Get ... string:synced`。

## 8. CLI（printerctl，纯 libdbus；默认 session bus，有 DBUS_SYSTEM_BUS_ADDRESS 时用 system）

```
printerctl [--bus ADDR] get <prop>            # 单属性(Properties.Get)
printerctl [--bus ADDR] set <prop> <value>    # 按 §2 类型表封送(s/i 属性自动判型)
printerctl [--bus ADDR] call <method> [args...]  # 方法表自动封送; Start_print_job 的
                                              #   末尾 k=v 对进 a{sv}(数字=i，否则=s)
printerctl [--bus ADDR] caps                  # 人可读打印当前选中打印机的全部能力
printerctl [--bus ADDR] monitor               # 订阅本服务全部信号，解码人读输出
printerctl [--bus ADDR] --get-all             # 全属性+当前值
```
示例：
```
printerctl get connection_state
printerctl set copies 3
printerctl call Scan_wifi                  # §20: 扫描周围网络(元素 "SSID\tRSSI(dBm)\tSEC")
printerctl get scanned_networks            # 同源属性(notify 已随扫描广播)
printerctl caps                            # 能力总览(media/尺寸/质量/色彩/格式/份数上限)
printerctl call Connect_to_ap "Mi Photo Printer 1S [A1B2]" 12345678
printerctl call Start_discovery
printerctl call Select_printer "Mi Photo Printer 1S [A1B2]"
printerctl get supported_media             # 选机后可见(探测发布)
printerctl set media oe_photo_3x3in        # 命中 supported_media 才被接受
printerctl call Start_print_job /tmp/a.jpg /tmp/b.jpg copies=2 print-quality=high print-scaling=fit
printerctl call Cancel_job
printerctl monitor
```
§18 扩展（v2.1）：`call Get_photo_preview <path> <max_w> <max_h>`、`call Get_photo_info <path>`、
`call Preview_print <path> k=v...`（与 Start_print_job 同款 k=v；`crop_*=小数` 封 d，
`rotation/copies=整数` 封 i，其余 s）。

## 9. 服务进程

- 二进制 `/data/x2print/bin/printer-service`（aarch64, NDK r25b 构建，链接设备 `/system/lib64/libdbus.so`）；同目录 `printerctl`、`selftest`、`selftest_img`、`wpa_supplicant`、`wpa_cli`、`odindb-cli`、`udhcpc.sh`、`x2_verify.sh`。生产部署/控制统一走根目录 `service.sh install|start|stop|status|log|ctl|sh|verify`（adb 通道，见 README）。
- 参数：`--config <path>`（默认 /data/x2print/config.ini）、`--mock`（强制 mock 后端；无真后端符号时自动回落 mock 并告警）、`--bus <address>`（显式总线地址）、`--log <file>`、`--loglevel <lvl>`、`--daemon`（fork 后台）、`--help/--version`。
- `service.sh start` 幂等（已运行跳过）；`stop` = SIGTERM 干净退出（存配置）。
- 后端接缝 `src/printer-service/backend.h`：函数指针表（connect/join/restore/discover/select/photo_list/print/cancel/tick）；阶段2 打印核心（src/net/）实现同表替换。
