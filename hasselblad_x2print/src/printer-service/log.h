/* log.h — 简单分级日志: stderr + 文件(滚动截断) */
#ifndef PRN_LOG_H
#define PRN_LOG_H

enum prn_log_level { PRN_LOG_DEBUG = 0, PRN_LOG_INFO, PRN_LOG_WARN, PRN_LOG_ERROR };

/* file==NULL 只输出 stderr; level 以下(含)才输出 */
void prn_log_init(const char *file, int level);
void prn_log_close(void);
void prn_log_raw(int level, const char *fmt, ...);

#define LOGD(...) prn_log_raw(PRN_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) prn_log_raw(PRN_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) prn_log_raw(PRN_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) prn_log_raw(PRN_LOG_ERROR, __VA_ARGS__)

/* 级别名 ↔ 数值, 失败返回 -1 */
int prn_log_level_parse(const char *name);

/* 递归建目录(成功或已存在=0), log.c 提供, config.c 复用 */
int prn_mkdir_p(const char *path);

#endif
