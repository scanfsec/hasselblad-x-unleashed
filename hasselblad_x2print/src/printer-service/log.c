/* log.c — 分级日志实现
 * 输出: stderr + 可选文件(默认 /data/x2print/log/service.log)
 * 滚动: 文件超过 PRN_LOG_MAX 时截断重写(freopen "w"), 运行中每次写前检查
 */
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define PRN_LOG_MAX (256 * 1024)   /* 256 KB 截断 */

static FILE *g_logf = NULL;
static int   g_level = PRN_LOG_INFO;
static char  g_path[512] = "";

static const char *lvl_name(int l)
{
    switch (l) {
    case PRN_LOG_DEBUG: return "D";
    case PRN_LOG_INFO:  return "I";
    case PRN_LOG_WARN:  return "W";
    default:            return "E";
    }
}

/* 递归建目录(a/b/c), 成功或已存在返回 0 */
int prn_mkdir_p(const char *path)
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
    return (mkdir(buf, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

void prn_log_init(const char *file, int level)
{
    g_level = level;
    if (!file || !*file) return;
    if (strlen(file) >= sizeof(g_path)) { fprintf(stderr, "log path too long\n"); return; }
    strcpy(g_path, file);

    /* 建父目录: 找最后一个 '/' */
    char dir[512];
    strcpy(dir, g_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (prn_mkdir_p(dir) != 0)
            fprintf(stderr, "log: mkdir %s failed: %s\n", dir, strerror(errno));
    }
    /* 超限截断: 以追加打开前检查大小 */
    struct stat st;
    if (stat(g_path, &st) == 0 && st.st_size > PRN_LOG_MAX)
        g_logf = fopen(g_path, "w");
    else
        g_logf = fopen(g_path, "a");
    if (!g_logf)
        fprintf(stderr, "log: open %s failed: %s\n", g_path, strerror(errno));
}

void prn_log_close(void)
{
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
}

void prn_log_raw(int level, const char *fmt, ...)
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
    /* 运行中滚动 */
    long sz = ftell(g_logf);
    if (sz > PRN_LOG_MAX) {
        FILE *nf = freopen(g_path, "w", g_logf);   /* 失败会关掉原流 */
        if (!nf) { g_logf = NULL; return; }
        g_logf = nf;
        fprintf(g_logf, "%s I log truncated (was %ld bytes)\n", ts, sz);
    }
    va_start(ap, fmt);
    fprintf(g_logf, "%s %s ", ts, lvl_name(level));
    vfprintf(g_logf, fmt, ap);
    fputc('\n', g_logf);
    fflush(g_logf);
    va_end(ap);
}

int prn_log_level_parse(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "debug")) return PRN_LOG_DEBUG;
    if (!strcmp(name, "info"))  return PRN_LOG_INFO;
    if (!strcmp(name, "warn"))  return PRN_LOG_WARN;
    if (!strcmp(name, "error")) return PRN_LOG_ERROR;
    return -1;
}
