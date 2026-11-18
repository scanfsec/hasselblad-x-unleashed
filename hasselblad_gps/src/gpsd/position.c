/* position.c — last-known 位置缓存实现(见 position.h; 单线程, 直接全局) */
#include "position.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

gps_pos_t g_pos;

long long gps_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void gps_pos_reset(void)
{
    memset(&g_pos, 0, sizeof g_pos);
}

void gps_pos_stamp(const char *source)
{
    g_pos.mono_ms = gps_now_ms();
    g_pos.ever_updated = 1;
    snprintf(g_pos.source, sizeof g_pos.source, "%s", source ? source : "?");
}

long long gps_pos_age_ms(void)
{
    if (!g_pos.ever_updated) return -1;
    return gps_now_ms() - g_pos.mono_ms;
}

/* gps_available 属性语义(X1D II 数据面可用, §1.4): 有过更新且未过新鲜窗 */
int gps_pos_available(void)
{
    if (!g_pos.ever_updated) return 0;
    return gps_pos_age_ms() <= (long long)g_cfg.max_age_s * 1000;
}

/* 注入门控: fix 有效且坐标未超龄(§6.2 #3 单门控简化) */
int gps_pos_fresh_for_inject(void)
{
    return g_pos.fix_valid && gps_pos_available();
}
