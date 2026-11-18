/* log.h — 分级滚动日志
 * 规格: docs/X2DII_PORT_ANALYSIS.md §4.2(/data/hasselblad_gps/log/gpsd.log,
 * 滚动上限 512KB×2: 超限改名 .1 续写, 旧 .1 丢弃); geotagd.sh 日志格式沿用
 * ("MM-DD HH:MM:SS L message", x2print log.c 同构)。
 */
#ifndef GPS_LOG_H
#define GPS_LOG_H

#define GPS_LOG_DEBUG 0
#define GPS_LOG_INFO  1
#define GPS_LOG_WARN  2
#define GPS_LOG_ERROR 3

int  gps_log_level_parse(const char *name);          /* -1=非法 */
void gps_log_init(const char *file, int level, int max_bytes);
void gps_log_close(void);                            /* flush + 关闭(SIGTERM 路径) */
void gps_log_raw(int level, const char *fmt, ...);

int  gps_mkdir_p(const char *path);                  /* 递归建目录(日志/运行目录用) */

#define LOGD(...) gps_log_raw(GPS_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) gps_log_raw(GPS_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) gps_log_raw(GPS_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) gps_log_raw(GPS_LOG_ERROR, __VA_ARGS__)

#endif
