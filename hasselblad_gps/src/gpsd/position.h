/* position.h — last-known 位置缓存(单线程事件循环, 无锁)
 * 规格: docs/GPS_LIFECYCLE_X1D_X1DII.md §6.2 #2/#3 —— X1D 的
 * lastKnownPosition 语义在 X2D II 上的重建: daemon 内存持有当前坐标,
 * 时间戳新鲜度判据(超 max_age_s 视为 stale 不写照片)。
 * 字段面对齐 SnapshotMetadata 七字段(§3.3): lat/lon/alt/fixValid/
 * groundSpeed/fixTimeStamp + 补充 dop/track/nsat(hginject 可用则用)。
 */
#ifndef GPS_POSITION_H
#define GPS_POSITION_H

#define GPS_POS_SOURCE_MAX 16
#define GPS_POS_UTC_MAX    40

typedef struct {
    double   lat, lon;             /* 度, WGS-84 */
    double   alt;                  /* 米 */
    double   dop;                  /* HDOP */
    double   speed;                /* m/s(Qt GroundSpeed 单位) */
    double   track;                /* 度, 真北 */
    unsigned nsat;                 /* 在用/可见卫星数 */
    int      fix_valid;            /* RMC 状态 A / GGA quality>0 / 外部源断言 */
    int      have_alt;             /* alt 字段有效(alt NaN 语义=无高程) */
    char     utc[GPS_POS_UTC_MAX]; /* "YYYY-MM-DD hh:mm:ss"(UTC) 或空 */
    char     source[GPS_POS_SOURCE_MAX]; /* dbus|file|nmea|simulate */
    long long mono_ms;             /* 本次更新时刻(CLOCK_MONOTONIC ms) */
    int      ever_updated;
} gps_pos_t;

extern gps_pos_t g_pos;

void      gps_pos_reset(void);
long long gps_now_ms(void);                 /* CLOCK_MONOTONIC ms */
void      gps_pos_stamp(const char *source);/* 置 mono_ms=now, ever_updated=1, source */
long long gps_pos_age_ms(void);             /* ever? age : -1 */
int       gps_pos_available(void);          /* 数据管线可用: 有过更新且未超 max_age_s */
int       gps_pos_fresh_for_inject(void);   /* fix_valid && age<=max_age_s */

#endif
