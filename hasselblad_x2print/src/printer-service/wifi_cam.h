/* wifi_cam.h — 相机侧 Wi-Fi STA 编排(NOTES §15 真机实测路径, backend_real 执行层)
 *
 * P0 反饿死重构(NOTES §22): 一切长命令(insmod/udhcpc/wpa_cli/iw/killall/rmmod)
 * 改 fork+exec 后台子进程, tick 里 waitpid(WNOHANG) 轮询 + 超时 SIGKILL ——
 * join/扫描/恢复三台状态机每次 step 只做「至多一次非阻塞轮询」, 主事件循环
 * 任何步骤间都可呼吸(D-Bus dispatch + 看门狗不停摆), 断开/取消可即时中断。
 *
 * join 阶段机(backend tick 每轮步进一次):
 *   wifi_cam_join_start(ssid, psk)  受理(0)/参数错(-1)
 *   wifi_cam_join_step()            0=joined 完成 / 1=进行中 / -1=失败(日志有细节)
 *   wifi_cam_join_phase()           人读阶段名(日志/观测)
 *
 * STA 序列(全部真机验证, X2D II + bcmdhd IAPSTA):
 *   1. 基线检测: /sys/class/net/wlan0 在? → 模块已载, 我们不拥有(绝不 rmmod);
 *      不在 → insmod /system/lib/modules/bcmdhd.ko(所有权=我们, 落 module.owned
 *      凭据; 恢复/下次启动 boot_cleanup 时 rmmod) — 3 次重试, 间隔 3s(SDIO 竞态)
 *   2. ifconfig wlan0 up
 *   3. 写 /data/x2print/wpa.conf(0600) → 自带静态 wpa_supplicant
 *      -B -Dnl80211 -i wlan0 -c ...(wl join 证死, 见 NOTES §14/§15)
 *   4. wpa_cli -p /data/x2print/wpasock status 轮询 wpa_state=COMPLETED(45s 预算)
 *   5. busybox udhcpc -i wlan0 -s <事件脚本> -n -q -t 4(取热点动态租约)
 *
 * 扫描机(§20; 同为 tick 步进):
 *   wifi_cam_scan_begin()           受理(0)/环境错(-1)
 *   wifi_cam_scan_step(&out,&n)     0=完成(*out=malloc 列表, 元素
 *                                   "SSID\tRSSI(dBm)\tSEC", 调用方 free)
 *                                   1=进行中 / -1=失败
 *   芯片保障与 join 共用; 扫描后保持芯片可用不拆除(看门狗/restore 收尾)。
 *
 * 恢复机(反序; 受理即返回, tick 推进至完成):
 *   wifi_cam_restore_begin()        先 abort 在途子进程/状态机, 再启动恢复序列
 *   wifi_cam_restore_step()         1=进行中 / 0=完成(幂等, 完成后重复调恒 0)
 *
 * 中断:
 *   wifi_cam_abort()                断开/取消置位: 在途子进程立即 SIGKILL,
 *                                   join/扫描机就地复位(恢复机由 restore_begin 重启)
 *
 * 残留清理(P1②, NOTES §22):
 *   wifi_cam_boot_cleanup()         服务启动一次性(同步, 幂等): 按 pid 文件+pgrep
 *                                   自带路径杀陈旧 wpa_supplicant、按 module.owned
 *                                   凭据 rmmod、wlan0 地址 flush(仅凭据在案时,
 *                                   绝不动原生 AP 的 wlan0)。全程日志。
 *
 * 恢复序列(反序): wpa_cli terminate → killall → ip addr flush dev wlan0 →
 *   ifconfig wlan0 down → rmmod bcmdhd(仅当我们 insmod 过, 凭据随之清除)。
 *
 * psk 安全: 只落 /data/x2print/psk 与 wpa.conf(均 chmod 0600),
 *   不进 D-Bus 属性 / 不进命令行(ps 可见) / 不进日志。
 */
#ifndef PRN_WIFI_CAM_H
#define PRN_WIFI_CAM_H

/* join 阶段机: start 受理后由 tick 反复调 step 直到 0/-1 */
int  wifi_cam_join_start(const char *ssid, const char *psk); /* 0=受理, -1=参数/环境错 */
int  wifi_cam_join_step(void);  /* 0=joined, 1=进行中, -1=失败 */
const char *wifi_cam_join_phase(void);                       /* 当前阶段名(静态串) */

/* §20 SSID 扫描选择(tick 步进): 结果元素 "SSID\tRSSI(dBm)\tSEC",
 * SEC=wpa2|open; 隐藏网络跳过, 同 SSID 去重取最强, RSSI 降序, ≤32 项。 */
int  wifi_cam_scan_begin(void);                       /* 0=受理, -1=环境错 */
int  wifi_cam_scan_step(char ***out, int *n);         /* 0=完成 1=进行中 -1=失败 */

/* 反序恢复相机 Wi-Fi 基线(异步步进, 幂等): 任何中间态调用都收敛; 尽力而为 */
void wifi_cam_restore_begin(void);
void wifi_cam_psk_purge(void);   /* 会话/崩溃残留 psk 清理 */
int  wifi_cam_restore_step(void);                     /* 1=进行中 0=完成 */

/* 中断位(Disconnect_and_restore/Cancel_job 置位): 在途子进程立即终结 */
void wifi_cam_abort(void);

/* 启动残留清理(P1②): 同步一次性, 幂等, 全程日志; 非 camera 环境无操作 */
int  wifi_cam_boot_cleanup(void);

/* 会话 wakelock(NOTES §19): join 受理起持有(内核命名锁 x2print-session,
 * /sys/power/wake_lock) —— 相机待机(autosleep 无锁即挂)会冻结 camera-system
 * 异步电源序列(power_on_wifi「受理但不起」)与 STA 链路; 恢复完成时由
 * backend_real 在合法电源同步之后释放, 服务启动时 release 一次清崩溃残留。
 * 非相机环境(WSL)为无操作桩。 */
void wifi_cam_wake_hold(void);
void wifi_cam_wake_release(void);

#endif
