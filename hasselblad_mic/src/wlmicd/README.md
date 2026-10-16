# wlmicd — DJI Mic 2 无线麦服务守护进程（零注入 wl 服务栈）

单进程独立 daemon，替代阶段 1 的 frida 注入引擎（miccore）。与 bt_bsa_app 的
全部交互只经过它**本来就在 main() 里原生创建**的两条 dsock 通道 —— 无 hook、
无 LD_PRELOAD、无 ptrace、无进程内调用、无内存写入。**bt_bsa_app 零修改。**

```
TX <===== SPP (私有 AA/Wl/DUSS 协议) =====> bt_bsa_app (100% 原生行为)
                                              │
        ┌─────────────────────────────────────┤
        │ /dsock/57012 上行 dgram              │ /dsock/57011 下行 dgram
        │ {01, seq8, len16be, bd[6], payload}  │ {01, seq8, len16be(=10+plen), bd[6], 裸DUSS≤125B}
        │ payload = N×125B 音频单元             │ bt_bsa_app 原生包 AA-ver1 帧发 SPP
        │  (6B{btclk u32le,intra u16le}+119B Wl)│
        ▼                                      │
  ┌========================= wlmicd =========================┐
  │ 57012 收线程 → 统一解析器(AA ver0/ver1/裸块)               │
  │   Wl 0x10 音频 → LC3plus 解码(ETSI, PLC) → mic.pcm / WAV  │
  │   Wl 0x19 状态 → TLV → battery/charging/mute/recording    │
  │ 57011 发送线程(RQ) → 0x57 状态询问(10s) / 0x55 保活(1Hz)   │
  │   / conn-OK 刷新 / 休眠通告 0x54{00,20}                    │
  │ bsa daemon socket 客户端 → scan/pair/unpair/RSSI(0x582)/   │
  │   DgClose(0x4B4)/TmDisconnect(0x583)                       │
  │ 三面接口(INTERFACE.md v3 不变): mic_ctl / mic_status.json / │
  │   mic.pcm —— GUI 零改动                                    │
  └===========================================================┘
```

## 为什么这等于"完整的 wl 服务栈"

Action 5 产品栈是 bt_bsa_app(传输) + dji_network(状态机/帧构造) +
libwirelessmicservice(协议解析/LC3/状态聚合) 三个进程。哈苏固件里后两者
不存在（`feature_bt_audio=0`），wlmicd 把它们的职责合并进一个进程，
挂在原生 dsock 通道上：

| 产品腿 | 产品实现 | wlmicd 实现 |
|---|---|---|
| SPP 建链 | TX HFP→SPP 特判 / WMS cmd 0x13 | **原生保留**（TX 主动 dance；WMS 通道 90000 源校验=80000，dji_network 占用，外部不可达 → connect=armed-passive） |
| 版本/时间同步握手 | bt_bsa_app 自带 | **原生保留**（写 bt_audio.cfg 后原生自驱，见下） |
| 上行音频→解码→PCM | wl service 收 57012 | 同：绑 /dsock/57012，125B 单元剥离，LC3plus→PCM FIFO/WAV |
| 状态 TLV→GUI | wl service 0x39a20 | 同：Wl 0x19 → TLV（battery=T10/charging=T05/mute=T01/recording=T06）→ status JSON |
| 维持流(0x55/0x56) | dji_network 被动应答 | 1Hz 0x55（RSSI 事件腿复刻）+ 原生 0x58 握手；实测 TX fw316 只需这两条 |
| 0x57 状态交换 | dji_network 应答 | **主动 10s 轮询**（16+6n 正确格式，btclk 取自音频单元头缓存） |
| conn-OK 链(0x65/03+0x54/10) | dji_network 五连 | 周期刷新（60s 可配）+ kickstart 命令 |
| 休眠通告(0x54{00,20}) | feature_bt_audio_stop | disconnect 前发+200ms 宽限 |
| 扫描/配对/删配对 | WMS→bt_bsa_app | bsa daemon socket 直连（0x0C8/0x12C/0x12D/0x131/0x134，任意客户端可用） |

## 原生行为边界（反汇编定案，fw316，全部有据）

- **上行路由** `_bt_audio_analysis_v1@0x21a48`：音频单元→57012；
  TX→相机 set12 的 **0x58/0x65 本地处理，其余（0x15/0x17/0x57/0x65-ACK）静默丢弃**；
  非相机地址 DUSS→WMS cmd 0x05(80000, 无人收)。⇒ TX 控制帧不会出现在 57012，
  wlmicd 的维持腿全部是**主动周期式**（反应式应答器保留在解析器里，仅当 DUSS
  真的搭 57012 载荷到达时触发——离线/未来固件）。
- **下行闸门** `_bt_audio_recv_thread@0x241e0`：dgram bd 必须匹配
  bt_audio.cfg 槽（内存结构 @0x800e90）且 slot+0x32(ver_done)≠0。
  ver_done 由 TX 应答相机 0x65/04 版本同步时设置（@0x226c8→0x22770），
  而相机的版本同步发送又被 is_master_addr(read_config) 闸在 cfg 存在性上。
  ⇒ **写好 bt_audio.cfg 后，下一次 SPP 建链自然完成版本同步，闸门即开**
  （cutover 模式自动维护该文件，产品 update_config 的等价复刻）。
- CRC16 修正：阶段 1 引擎的 duss_crc16 是退化转写错误（不同输入得到相同值，
  TX 容忍了错误 CRC）。wlmicd 用标准查表式，六个已验证锚点（真实抓包帧
  58/02→03EE、报告模板 0x54→1464 / 0x55→CB49 / 0x57→2A99 / ccode→E08D /
  ver-sync→B9C8）全部逐字节复现。**miccore.c 源码已标注该 bug 但未重部署。**

## 运行

```
adb shell /data/mic/wlmicd -d        # 守护进程（-d 后台；-v 日志也上 stderr）
adb shell /data/mic/micctl status    # 面板不变（cutover 后）
# 离线自检（不绑任何 dsock/socket，可与活引擎共存）
adb shell /data/mic/wlmicd --probe57                     # 0x57/0x55 字节模板校验
adb shell "cd /data/mic && ./wlmicd --replay spp_audio_capture.bin \
           --expect-frames 28829 --tlv-synth --wav"      # 全链重放
```

### 兼容期（compat，默认自动检测）

启动时探测 /data/mic/mic_ctl：若阶段 1 引擎应答 → compat=1——
- 面板走旁路：wlmicd_ctl / wlmicd_status.json（不动 mic_ctl/mic_status.json/mic.pcm）
- 硬钳位：keepalive_055/responder/auto_connect/linkup_refresh/manage_bt_audio_cfg/
  poll_057 全部关闭（避免与引擎双答；0x57 轮询在闸门未开时只会被原生丢弃）
- 只做：**57012 镜像消费**（音频解码+TLV 四字段观测，写旁路 status）+ RSSI 轮询

### 切换（cutover，mic.cfg `compat=0` 或引擎退场后自动）

1. mic.cfg 加 `compat=0`（或等引擎消失后自动检测）
2. wlmicd 接管三个规范面板 + 写 bt_audio.cfg
3. **下一次 TX 重连**完成原生版本同步 → 57011 闸门开 → 0x57 轮询/conn-OK
   刷新/kickstart 全部生效
4. 撤掉 frida（不再 attach）；bt_bsa_app 下次自然重启后无需任何注入

## mic.cfg 配置管理（wlmicd 独占写权）

双 mic 向前兼容格式（`bd=` 为遗留拼写，始终与 bd0= 同步保留）：

```
bd=e4:7a:2c:f2:53:69      # mic0（遗留行，老引擎只认这个）
bd0=e4:7a:2c:f2:53:69     # mic0（规范拼写）
bd1=                      # mic1 预留（写入 bt_audio.cfg 第二槽）
auto_connect=1  keepalive_055=1  passive=1     # ← 两边共享的四个键
compat=0  poll_057=1  poll_057_sec=10  responder=1
linkup_refresh_sec=60  manage_bt_audio_cfg=1  rssi_poll=1  adopt_bd=1
```

- **pair 成功 → 自动改写 bd=/bd0= 为新地址**（其余行原样保留，原子替换；
  adopt_bd=0 可关）。未配置 bd 的来流只计数不解码（日志提示 pair 采纳）。
- **热生效**：hk 线程每秒盯 mtime，变化即重读（ctl `reload` 强制）——
  改 poll_057_sec/keepalive_055/bd 等不用重启。
- **迁移期所有权**：两边读同一文件；只有 wlmicd 写。共享四键的归属——
  `bd/auto_connect` 由 wlmicd 采纳维护（pair 时）；`keepalive_055` 兼容期
  归引擎（wlmicd 硬钳位不理会文件值），cutover 后归 wlmicd；`passive`
  仅引擎认识（wlmicd 架构上天然 passive，忽略之）。老引擎解析器跳过
  未知行，wlmicd 追加的键对它无害。

## 离线验证记录（2026-10-03，真机 /data/mic）

| 用例 | 结果 |
|---|---|
| `--probe57/--probe55` 字节模板 | **PASS**（与 REPORT_ctrl_frames §4 逐字节一致，含修正后 CRC16） |
| 3.6MB 捕获重放（5766 dgram） | frames **28830/28830，drops 0**（比裸喂引擎多找回全部块；参考 WAV 差异仅 8,245B，全部位于参考自身的补帧点） |
| 合成 Wl 0x19 TLV 注入（真 57012 dgram 路径） | **battery=82 charging=1 mute=0 recording=1 全亮** |
| AA 帧 live dump（feeddump，含 DUSS 尾帧）重放 | 解析器+应答器触发正确：TX 0x65/00 → conn-OK 对（27B+21B，CRC 正确） |
| 22.7MB 全量压力（35,963 dgram） | frames **179,815/179,815，drops 0**，17.6s（>10× 实时余量） |

## 已知边界

- `connect` 是 armed-passive（等 TX 原生 dance），不能主动 DgOpen——
  WMS 命令面源校验所限；TX 一旦醒着回连即恢复。
- cutover 的 disconnect 用 DgClose(handle=bt-dg-socket<N> 序号)+TmDisconnect；
  双 mic 同时在线时 handle 归属未细化（当前单 mic 场景无影响）。
- ccode(0x65/02) 不发送：fw316 TX 无 ccode 也推流（PASSIVE 会话实证），
  且其原生触发(WMS cmd 0x19)外部不可达。
