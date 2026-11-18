/* nmea.c — RMC/GGA 解析实现(见 nmea.h; X1D II gpsd @0xce9c RMC 正则的 C 等价物)
 * 句式(逗号分割, 索引从句型标识后起):
 *   RMC: 1=UTC hhmmss.sss 2=status A/V 3=lat 4=N/S 5=lon 6=E/W
 *        7=speed(knots) 8=track(deg) 9=date ddmmyy
 *   GGA: 1=UTC 2=lat 3=N/S 4=lon 5=E/W 6=quality(0=nofix) 7=nsat
 *        8=hdop 9=alt 10='M'
 * 速度 knots→m/s(×0.514444, Qt GroundSpeed 单位约定,
 * docs/GPS_LIFECYCLE_X1D_X1DII.md §3.3)。
 */
#include "nmea.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define NMEA_MAX_FIELD 16
#define NMEA_TOK_LEN  16

/* 校验和: "$" 与 "*" 之间字节异或; 无完整 "*HH" 不校验(兼容截断源) */
static int checksum_ok(const char *line)
{
    const char *star = strchr(line, '*');
    if (!star || !star[1] || !star[2]) return 1;
    int sum = 0;
    for (const char *p = line + 1; p < star; p++) sum ^= (unsigned char)*p;
    int hexv = 0;
    for (int i = 1; i <= 2; i++) {
        char c = (char)tolower((unsigned char)star[i]);
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        hexv = hexv * 16 + d;
    }
    return sum == hexv;
}

double nmea_to_deg(const char *val, char hemi)
{
    if (!val || !*val) return 0.0;
    double raw = atof(val);
    int deg = (int)(raw / 100.0);
    double minutes = raw - deg * 100.0;
    double d = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') d = -d;
    return d;
}

/* 按逗号切字段(跳过句型标识), tok 借用定长缓冲 */
static int split_fields(const char *line, char tok[][NMEA_TOK_LEN], int maxtok)
{
    const char *p = strchr(line, ',');
    if (!p) return 0;
    p++;
    int n = 0;
    while (p && *p && *p != '*' && n < maxtok) {
        const char *e = p;
        while (*e && *e != ',' && *e != '*' && *e != '\r' && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        if (len >= NMEA_TOK_LEN) len = NMEA_TOK_LEN - 1;
        memcpy(tok[n], p, len);
        tok[n][len] = 0;
        n++;
        p = (*e == ',') ? e + 1 : NULL;
    }
    return n;
}

static int two_digits(const char *s)
{
    if (!s || !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return -1;
    return (s[0] - '0') * 10 + (s[1] - '0');
}

/* RMC/GGA 时间/日期 → "YYYY-MM-DD hh:mm:ss"(UTC); 日期跨句记忆
 * (GGA 常先于 RMC 到, 只有时刻没有日期 → 用上一句 RMC 记下的日期) */
static int s_yy = -1, s_mo = -1, s_dd = -1;

static void note_time(const char *hhmmss, const char *ddmmyy, char *out, size_t outsz)
{
    int hh = 0, mm = 0, ss = 0;
    if (hhmmss && strlen(hhmmss) >= 6) {
        hh = two_digits(hhmmss);
        mm = two_digits(hhmmss + 2);
        ss = two_digits(hhmmss + 4);
        if (hh < 0) hh = 0;
        if (mm < 0) mm = 0;
        if (ss < 0) ss = 0;
    }
    if (ddmmyy && strlen(ddmmyy) >= 6) {
        int dd = two_digits(ddmmyy);
        int mo = two_digits(ddmmyy + 2);
        int yy = two_digits(ddmmyy + 4);
        if (dd >= 0) s_dd = dd;
        if (mo >= 0) s_mo = mo;
        if (yy >= 0) s_yy = yy;
    }
    if (hh || mm || ss || s_yy >= 0)
        snprintf(out, outsz, "%04d-%02d-%02d %02d:%02d:%02d",
                 s_yy >= 0 ? 2000 + s_yy : 2000, s_mo > 0 ? s_mo : 1,
                 s_dd > 0 ? s_dd : 1, hh, mm, ss);
}

int nmea_parse(const char *line, gps_pos_t *pos, unsigned *fields)
{
    if (!line || line[0] != '$') return -1;
    if (strlen(line) > 90) return -1;                 /* 帧上限 82+NMEA 头尾余量 */
    if (!checksum_ok(line)) return -1;

    /* 句型 = 跳过 2 字符 talker 后的 3 字符(GP/GN/GL/GA + RMC/GGA) */
    const char *type = line + 3;
    int is_rmc = (strncmp(type, "RMC", 3) == 0);
    int is_gga = (strncmp(type, "GGA", 3) == 0);
    if (!is_rmc && !is_gga) return -1;

    char tok[NMEA_MAX_FIELD][NMEA_TOK_LEN];
    int n = split_fields(line, tok, NMEA_MAX_FIELD);

    if (is_rmc) {
        /* tok[i] = 第 i+1 字段: 0=time 1=status 2=lat 3=NS 4=lon 5=EW
         *                     6=speed 7=track 8=date */
        if (n < 7) return -1;
        char status_c = tok[1][0];
        if (status_c != 'A' && status_c != 'V') return -1;

        unsigned f = NMEA_F_FIX;
        pos->fix_valid = (status_c == 'A');
        if (pos->fix_valid && tok[2][0] && tok[4][0]) {
            pos->lat = nmea_to_deg(tok[2], tok[3][0]);
            pos->lon = nmea_to_deg(tok[4], tok[5][0]);
            f |= NMEA_F_LATLON;
            if (tok[6][0]) {
                pos->speed = atof(tok[6]) * 0.514444; /* knots → m/s */
                f |= NMEA_F_SPEED;
            }
            if (n > 7 && tok[7][0]) {
                pos->track = atof(tok[7]);
                f |= NMEA_F_TRACK;
            }
        }
        note_time(tok[0], (n > 8) ? tok[8] : NULL, pos->utc, sizeof pos->utc);
        f |= NMEA_F_UTC;
        *fields = f;
        return 0;
    }

    /* GGA tok[i] = 第 i+1 字段: 0=time 1=lat 2=NS 3=lon 4=EW 5=quality
     *                        6=nsat 7=hdop 8=alt */
    if (n < 7) return -1;
    int quality = atoi(tok[5]);
    unsigned f = NMEA_F_FIX;
    pos->fix_valid = (quality > 0);
    if (n > 6 && tok[6][0]) {
        pos->nsat = (unsigned)atoi(tok[6]);
        f |= NMEA_F_NSAT;
    }
    if (n > 7 && tok[7][0]) {
        pos->dop = atof(tok[7]);
        f |= NMEA_F_DOP;
    }
    if (n > 8 && tok[8][0]) {
        pos->alt = atof(tok[8]);
        pos->have_alt = 1;
        f |= NMEA_F_ALT;
    }
    if (pos->fix_valid && tok[1][0] && tok[3][0]) {
        pos->lat = nmea_to_deg(tok[1], tok[2][0]);
        pos->lon = nmea_to_deg(tok[3], tok[4][0]);
        f |= NMEA_F_LATLON;
    }
    note_time(tok[0], NULL, pos->utc, sizeof pos->utc);
    f |= NMEA_F_UTC;
    *fields = f;
    return 0;
}
