/* nmea.h — 极简 NMEA 解析器(RMC/GGA, talker 不校验)
 * 规格: docs/X2DII_PORT_ANALYSIS.md §2 数据源(c); 对齐 X1D II gpsd 行为
 * (docs/GPS_LIFECYCLE_X1D_X1DII.md §1.2.3): RMC 只认状态 A; GGA quality>0;
 * talker 前缀(GP/GN/GL/GA...)不参与匹配; 帧长上限 82 字符。
 * 校验和: 行尾带 *HH 时强校验, 不匹配整行丢弃。
 */
#ifndef GPS_NMEA_H
#define GPS_NMEA_H

#include "position.h"

/* 解析成功置位的字段位掩码 */
#define NMEA_F_LATLON (1u << 0)
#define NMEA_F_FIX    (1u << 1)
#define NMEA_F_SPEED  (1u << 2)
#define NMEA_F_TRACK  (1u << 3)
#define NMEA_F_ALT    (1u << 4)
#define NMEA_F_NSAT   (1u << 5)
#define NMEA_F_DOP    (1u << 6)
#define NMEA_F_UTC    (1u << 7)

/* 解析一行 NMEA(不以 '$' 开头/类型不识/校验和坏 → 返回 -1)。
 * 成功返回 0 并只改写 *pos 中 *fields 置位的字段(调用方先拷贝快照);
 * RMC V 状态 → NMEA_F_FIX 置位且 pos->fix_valid=0(显式无定位)。 */
int nmea_parse(const char *line, gps_pos_t *pos, unsigned *fields);

/* ddmm.mmmm / dddmm.mmmm → 十进制度(带 N/S/E/W 换算), 失败返回 0 */
double nmea_to_deg(const char *val, char hemi);

#endif
