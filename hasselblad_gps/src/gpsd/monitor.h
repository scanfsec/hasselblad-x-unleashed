/* monitor.h — 照片监视/注入管线(X2D II 无官方"照片落盘"钩子的重建)
 * 规格: docs/X2DII_PORT_ANALYSIS.md §1.3(事件+轮询双轨)与 §4.2:
 *   [触发层] 订阅 /camera /storage propertiesChanged(小写方言)宽匹配 →
 *            exposures_processing_counter / storage_processing_counter
 *            变化 → 延迟 event_delay_ms(2s) 扫描
 *   [兜底层] 无论信号是否到达, poll_interval_ms(2s) 定时扫描 DCIM
 *   [稳定层] 候选 1.5s 两次尺寸一致才注入(exFAT 直写大文件的最后一道保险)
 *   [防重]   done.list 启动全量 seed 存量(x2gps 21 张误标教训)+ 追加
 *   [门控]   fix_valid=0 或坐标 age>max_age_s → 跳过并记日志(不标 done)
 *   [注入]   exec hginject <file> --lat --lon [--alt], 超时 60s SIGKILL
 * gps_enable/gps_disable 对应本管线启停(disable=只缓存不写照片)。
 */
#ifndef GPS_MONITOR_H
#define GPS_MONITOR_H

void gps_monitor_init(void);        /* 建 done.list(加载到内存哈希) */
void gps_monitor_seed(void);        /* 启动全量标记存量照片(只标记不注入) */
void gps_monitor_tick(long long now); /* 周期扫描+稳定复核+子进程收割/超时 */
void gps_monitor_event_poke(void);  /* counter 信号到达 → 调度延迟扫描 */
void gps_monitor_set_enabled(int on);
int  gps_monitor_enabled(void);
int  gps_monitor_injecting(void);   /* 注入子进程在跑(诊断) */
void gps_monitor_shutdown(void);    /* kill 子进程 + flush done.list */

#endif
