/* log.c — 分级滚动日志实现(stderr + 文件双写)
 * 滚动策略(规格 §4.2 512KB×2): 当前文件超 max_bytes 时改名为 <file>.1
 * (旧 .1 先删), 重新打开续写; 任何时刻磁盘上至多 2 份日志。
 * 每行 fflush —— SIGTERM 干净退出要求日志即时落盘(log_flush 语义)。
 */
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static FILE *g_logf = NULL;
static int   g_level = GPS_LOG_INFO;
static int   g_max_bytes = 512 * 1024;
static char  g_path[512] = "";
static char  g_path1[520] = "";                      /* <file>.1 */

static const char *lvl_name(int l)
{
    switch (l) {
    case GPS_LOG_DEBUG: return "D";
    case GPS_LOG_INFO:  return "I";
    case GPS_LOG_WARN:  return "W";
    default:            return "E";
    }
}

int gps_mkdir_p(const char *path)
{
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' && p[1] && p[1] != '/') {
            *p = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) { *p = '/'; return -1; }
            *p = '/';
        }
    }
    return (mkdir(path, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

void gps_log_init(const char *file, int level, int max_bytes)
{
    g_level = level;
    if (max_bytes > 0) g_max_bytes = max_bytes;
    if (!file || !*file) return;
    if (strlen(file) >= sizeof(g_path)) { fprintf(stderr, "log path too long\n"); return; }
    strcpy(g_path, file);
    snprintf(g_path1, sizeof(g_path1), "%s.1", file);

    char dir[512];
    strcpy(dir, g_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (gps_mkdir_p(dir) != 0)
            fprintf(stderr, "log: mkdir %s failed: %s\n", dir, strerror(errno));
    }
    g_logf = fopen(g_path, "a");
    if (!g_logf)
        fprintf(stderr, "log: open %s failed: %s\n", g_path, strerror(errno));
}

void gps_log_close(void)
{
    if (g_logf) { fflush(g_logf); fclose(g_logf); g_logf = NULL; }
}

/* 超限滚动: gpsd.log -> gpsd.log.1(旧 .1 删除), 恰保留两份 */
static void rotate_if_needed(long sz)
{
    if (sz <= g_max_bytes) return;
    fclose(g_logf);
    remove(g_path1);                                 /* 丢最旧一份 */
    rename(g_path, g_path1);
    g_logf = fopen(g_path, "w");
    if (g_logf)
        fprintf(g_logf, "log rotated (was %ld bytes)\n", sz);
}

void gps_log_raw(int level, const char *fmt, ...)
{
    if (level < g_level) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s %s ", ts, lvl_name(level));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);

    if (!g_logf) return;
    long sz = ftell(g_logf);
    if (sz > g_max_bytes) {
        rotate_if_needed(sz);
        if (!g_logf) return;
    }
    va_start(ap, fmt);
    fprintf(g_logf, "%s %s ", ts, lvl_name(level));
    vfprintf(g_logf, fmt, ap);
    fputc('\n', g_logf);
    fflush(g_logf);
    va_end(ap);
}

int gps_log_level_parse(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "debug")) return GPS_LOG_DEBUG;
    if (!strcmp(name, "info"))  return GPS_LOG_INFO;
    if (!strcmp(name, "warn"))  return GPS_LOG_WARN;
    if (!strcmp(name, "error")) return GPS_LOG_ERROR;
    return -1;
}
