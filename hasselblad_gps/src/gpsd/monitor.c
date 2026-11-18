/* monitor.c — 照片监视/注入管线实现(见 monitor.h)
 * 结构: done 集合(哈希, 对应 done.list 文件) + pending 数组(候选, 定长小表)
 * + 单飞行注入子进程(fork/execv hginject, 60s 超时 SIGKILL, 退出后标 done)。
 * 绝不修改/删除照片本体; 注入动作全部交给 hginject(inject/ 并行开发线)。
 */
#include "monitor.h"
#include "config.h"
#include "position.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

#define PENDING_MAX 256
#define DONE_BUCKETS 8192

/* ---------- 极简字符串集合(链地址哈希) ---------- */
typedef struct sset_ent {
    struct sset_ent *next;
    char *k;
} sset_ent;

typedef struct {
    sset_ent **b;
    int nb, n;
} sset_t;

static int sset_init(sset_t *s, int nb)
{
    s->b = calloc((size_t)nb, sizeof *s->b);
    if (!s->b) return -1;
    s->nb = nb;
    s->n = 0;
    return 0;
}

static unsigned sset_hash(const char *k)
{
    unsigned h = 2166136261u;
    while (*k) { h ^= (unsigned char)*k++; h *= 16777619u; }
    return h;
}

static int sset_has(const sset_t *s, const char *k)
{
    if (!s->b) return 0;
    for (sset_ent *e = s->b[sset_hash(k) % (unsigned)s->nb]; e; e = e->next)
        if (!strcmp(e->k, k)) return 1;
    return 0;
}

static int sset_add(sset_t *s, const char *k)     /* 1=新增 0=已在 */
{
    if (!s->b) return 0;
    unsigned i = sset_hash(k) % (unsigned)s->nb;
    for (sset_ent *e = s->b[i]; e; e = e->next)
        if (!strcmp(e->k, k)) return 0;
    sset_ent *e = malloc(sizeof *e);
    if (!e) return 0;
    e->k = strdup(k);
    if (!e->k) { free(e); return 0; }
    e->next = s->b[i];
    s->b[i] = e;
    s->n++;
    return 1;
}

static void sset_clear(sset_t *s)
{
    if (!s->b) return;
    for (int i = 0; i < s->nb; i++) {
        sset_ent *e = s->b[i];
        while (e) { sset_ent *nx = e->next; free(e->k); free(e); e = nx; }
        s->b[i] = NULL;
    }
    s->n = 0;
}

/* ---------- 模块状态 ---------- */
typedef struct {
    char path[PATH_MAX];
    long long size;
    long long since;                 /* 最近一次尺寸变化时刻(ms) */
} pending_t;

static sset_t        s_done;
static sset_t        s_skiplog;     /* 无 fix 跳过的日志去重(防刷屏) */
static pending_t     s_pending[PENDING_MAX];
static int           s_pending_n;
static FILE         *s_donef;
static int           s_enabled = 1;
static long long     s_last_scan_ms = -1;
static long long     s_scan_due_ms; /* 信号触发的延迟扫描时刻(0=无调度) */
static pid_t         s_child;       /* 0=无飞行注入 */
static char          s_child_path[PATH_MAX];
static long long     s_child_start;
static int           s_child_killed;
static int           s_seeded;

/* ---------- done.list ---------- */
static void done_add(const char *path)
{
    if (!sset_add(&s_done, path)) return;            /* 已在(幂等) */
    if (s_donef) {
        fprintf(s_donef, "%s\n", path);
        fflush(s_donef);                             /* 即时落盘(断电安全) */
    }
}

/* ---------- 候选管理 ---------- */
static pending_t *pending_find(const char *path)
{
    for (int i = 0; i < s_pending_n; i++)
        if (!strcmp(s_pending[i].path, path)) return &s_pending[i];
    return NULL;
}

static void pending_drop(pending_t *p)
{
    int i = (int)(p - s_pending);
    if (i < 0 || i >= s_pending_n) return;
    memmove(&s_pending[i], &s_pending[i + 1],
            sizeof s_pending[0] * (size_t)(s_pending_n - i - 1));
    s_pending_n--;
}

static int ext_match(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char e[8];
    int i = 0;
    for (const char *p = dot + 1; *p && i < 7; p++)
        e[i++] = (char)tolower((unsigned char)*p);
    e[i] = 0;
    return !strcmp(e, "3fr") || !strcmp(e, "jpg") ||
           !strcmp(e, "heic") || !strcmp(e, "hif");
}

/* ---------- 目录扫描(递归, 深度限制) ---------- */
static void scan_dir(const char *dir, int depth, int seed_mode)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >= (int)sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 8) scan_dir(full, depth + 1, seed_mode);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !ext_match(de->d_name)) continue;
        if (seed_mode) {
            done_add(full);                          /* 启动 seed: 存量全标记 */
            continue;
        }
        if (sset_has(&s_done, full)) continue;
        pending_t *p = pending_find(full);
        if (p) continue;                             /* 已在观察 */
        if (s_pending_n >= PENDING_MAX) {
            LOGW("pending 满(%d), 本轮放弃 %s", PENDING_MAX, full);
            continue;
        }
        p = &s_pending[s_pending_n++];
        snprintf(p->path, sizeof p->path, "%s", full);
        p->size = (long long)st.st_size;
        p->since = gps_now_ms();
    }
    closedir(d);
}

static void do_scan(long long now, int seed_mode)
{
    scan_dir(g_cfg.dcim_root, 0, seed_mode);
    (void)now;
}

/* ---------- 注入子进程 ---------- */
static void spawn_inject(const char *path)
{
    char lat[32], lon[32], alt[32], dop[32], spd[32], trk[32], nsv[16];
    char utcb[24];   /* 归一化后 --utc 参数, 生命周期须覆盖 execv */
    snprintf(lat, sizeof lat, "%.7f", g_pos.lat);
    snprintf(lon, sizeof lon, "%.7f", g_pos.lon);
    /* hginject 选项子集: lat/lon 必传, 其余字段在位才传(DOP/Speed/Track
     * 仅提供时写入 EXIF——见 inject/README.md 标签策略; speed 单位换算
     * m/s(Qt GroundSpeed)→km/h(hginject CLI 口径)。fix_valid 不传:
     * 到达本函数前已过 gps_pos_fresh_for_inject() 门控, hginject 默认
     * 写 Status=A。 */
    char *argv[24];
    int ai = 0;
    argv[ai++] = (char *)g_cfg.inject_cmd;
    argv[ai++] = (char *)path;
    argv[ai++] = (char *)"--lat";
    argv[ai++] = lat;
    argv[ai++] = (char *)"--lon";
    argv[ai++] = lon;
    if (g_pos.have_alt) {
        snprintf(alt, sizeof alt, "%.2f", g_pos.alt);
        argv[ai++] = (char *)"--alt";
        argv[ai++] = alt;
    }
    if (g_pos.dop > 0.0) {
        snprintf(dop, sizeof dop, "%.1f", g_pos.dop);
        argv[ai++] = (char *)"--dop";
        argv[ai++] = dop;
    }
    if (g_pos.speed > 0.0) {
        snprintf(spd, sizeof spd, "%.1f", g_pos.speed * 3.6);
        argv[ai++] = (char *)"--speed";
        argv[ai++] = spd;
    }
    if (g_pos.track > 0.0) {
        snprintf(trk, sizeof trk, "%.1f", g_pos.track);
        argv[ai++] = (char *)"--track";
        argv[ai++] = trk;
    }
    if (g_pos.nsat > 0) {
        snprintf(nsv, sizeof nsv, "%u", g_pos.nsat);
        argv[ai++] = (char *)"--nsat";
        argv[ai++] = nsv;
    }
    if (g_pos.utc[0]) {
        /* gpsd 内部口径 "YYYY-MM-DD hh:mm:ss"(19B) → hginject 要
         * "YYYY-MM-DDTHH:MM:SSZ"(20B, parse_utc 严格校验长度/结尾 Z) */
        size_t ul = strlen(g_pos.utc);
        if (ul == 19) {
            memcpy(utcb, g_pos.utc, 19);
            utcb[10] = 'T';
            utcb[19] = 'Z';
            utcb[20] = '\0';
            argv[ai++] = (char *)"--utc";
            argv[ai++] = utcb;
        } else if (ul == 20) {
            argv[ai++] = (char *)"--utc";
            argv[ai++] = g_pos.utc;
        }
    }
    argv[ai] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork 注入失败: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(g_cfg.inject_cmd, argv);
        _exit(127);                                  /* exec 失败 */
    }
    s_child = pid;
    snprintf(s_child_path, sizeof s_child_path, "%s", path);
    s_child_start = gps_now_ms();
    s_child_killed = 0;
    LOGI("注入 %s (pid=%d lat=%s lon=%s%s)", path, (int)pid, lat, lon,
         g_pos.have_alt ? " alt 有" : "");
}

static void child_reap(long long now)
{
    if (s_child <= 0) return;

    /* 超时看门狗: 超时 SIGKILL(下一轮 tick 收尸标 done) */
    if (!s_child_killed &&
        now - s_child_start > (long long)g_cfg.inject_timeout_s * 1000) {
        LOGE("注入超时(>%ds), SIGKILL pid=%d %s",
             g_cfg.inject_timeout_s, (int)s_child, s_child_path);
        kill(s_child, SIGKILL);
        s_child_killed = 1;
    }

    int st = 0;
    pid_t r = waitpid(s_child, &st, WNOHANG);
    if (r == 0) return;                              /* 还在跑 */
    if (r < 0) {
        if (errno == EINTR) return;
        LOGE("waitpid %d: %s", (int)s_child, strerror(errno));
        done_add(s_child_path);                      /* 收不了尸也防重 */
        s_child = 0;
        return;
    }
    if (s_child_killed)           LOGE("注入失败(超时) %s", s_child_path);
    else if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                                 LOGI("OK   %s", s_child_path);
    else if (WIFEXITED(st))      LOGW("ERR  %s (hginject exit=%d)", s_child_path,
                                          WEXITSTATUS(st));
    else                          LOGW("ERR  %s (hginject 信号终止)", s_child_path);
    done_add(s_child_path);                          /* 尝试过即标 done(geotagd 语义) */
    s_child = 0;
}

/* ---------- 对外接口 ---------- */
void gps_monitor_init(void)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_cfg.done_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; gps_mkdir_p(dir); }

    if (sset_init(&s_done, DONE_BUCKETS) != 0 || sset_init(&s_skiplog, 256) != 0) {
        LOGE("done/skiplog 哈希分配失败");
        return;
    }
    /* 加载已有 done.list */
    FILE *f = fopen(g_cfg.done_path, "r");
    if (f) {
        char line[PATH_MAX];
        while (fgets(line, sizeof line, f)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (line[0] == '/') sset_add(&s_done, line);
        }
        fclose(f);
        LOGI("done.list 加载 %d 条", s_done.n);
    }
    s_donef = fopen(g_cfg.done_path, "a");
    if (!s_donef)
        LOGW("done.list 追加打开失败(%s): %s —— 防重将只在本进程内生效",
             g_cfg.done_path, strerror(errno));
}

void gps_monitor_seed(void)
{
    if (s_seeded) return;
    int before = s_done.n;
    do_scan(gps_now_ms(), 1);
    s_seeded = 1;
    LOGI("seed 完成: 存量标记 %d 条(done.list 共 %d) —— 只处理启动后的新照片",
         s_done.n - before, s_done.n);
}

void gps_monitor_event_poke(void)
{
    long long due = gps_now_ms() + g_cfg.event_delay_ms;
    if (s_scan_due_ms == 0 || due < s_scan_due_ms) s_scan_due_ms = due;
}

void gps_monitor_set_enabled(int on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    LOGI("注入管线 %s(%s)", on ? "启用" : "停用",
         on ? "新照片将写入 GPS" : "只缓存位置, 不写照片");
}

int  gps_monitor_enabled(void)        { return s_enabled; }
int  gps_monitor_injecting(void)      { return s_child > 0; }

void gps_monitor_tick(long long now)
{
    child_reap(now);

    /* 信号触发的延迟扫描 */
    if (s_scan_due_ms != 0 && now >= s_scan_due_ms) {
        s_scan_due_ms = 0;
        if (s_enabled) do_scan(now, 0);
    }
    /* 兜底定时扫描 */
    if (s_enabled && (s_last_scan_ms < 0 ||
                      now - s_last_scan_ms >= g_cfg.poll_interval_ms)) {
        s_last_scan_ms = now;
        do_scan(now, 0);
    }

    /* 稳定复核: 尺寸两次一致(间隔≥stable_ms)才可注入 */
    if (!s_enabled) return;
    int i = 0;
    while (i < s_pending_n) {
        pending_t *p = &s_pending[i];
        struct stat st;
        if (stat(p->path, &st) != 0) {
            /* 文件消失(被相机清理/移动): 丢弃候选, 不标 done */
            LOGD("候选消失 %s", p->path);
            pending_drop(p);
            continue;
        }
        long long sz = (long long)st.st_size;
        if (sz != p->size) {
            p->size = sz;                            /* 还在写, 重新计时 */
            p->since = now;
            i++;
            continue;
        }
        if (now - p->since < g_cfg.stable_ms) { i++; continue; }

        /* 尺寸稳定 → 门控检查 */
        if (!g_pos.fix_valid) {
            if (sset_add(&s_skiplog, p->path))
                LOGW("skip %s (无有效定位 fix_valid=0)", p->path);
            i++;
            continue;
        }
        long long age = gps_pos_age_ms();
        if (age > (long long)g_cfg.max_age_s * 1000) {
            if (sset_add(&s_skiplog, p->path))
                LOGW("skip %s (坐标龄 %llds > max_age_s %d)", p->path,
                     age / 1000, g_cfg.max_age_s);
            i++;
            continue;
        }
        if (s_child > 0) { i++; continue; }          /* 单飞行: 下轮再试 */
        pending_t victim = *p;
        spawn_inject(victim.path);
        pending_drop(p);                             /* 从候选除名(done 在收割时标) */
    }

    /* fix 恢复有效时清跳过日志(后续同名文件可再告警) */
    if (g_pos.fix_valid && s_skiplog.n > 4096) sset_clear(&s_skiplog);
}

void gps_monitor_shutdown(void)
{
    if (s_child > 0) {
        LOGI("退出: 终止注入子进程 pid=%d", (int)s_child);
        kill(s_child, SIGTERM);
        for (int i = 0; i < 30; i++) {
            int st;
            pid_t r = waitpid(s_child, &st, WNOHANG);
            if (r == s_child) { s_child = 0; break; }
            if (r < 0 && errno != EINTR) { s_child = 0; break; }
            usleep(50 * 1000);
        }
        if (s_child > 0) {
            kill(s_child, SIGKILL);
            int st;
            while (waitpid(s_child, &st, 0) < 0 && errno == EINTR) { /* 收尸 */ }
            s_child = 0;
        }
    }
    if (s_donef) {
        fflush(s_donef);
        fclose(s_donef);
        s_donef = NULL;
    }
}
