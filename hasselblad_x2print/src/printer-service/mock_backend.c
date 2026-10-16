/* mock_backend.c — 假后端: 模拟连热点耗时/发现列表/打印进度(2%/秒)
 * 供 WSL 与 GUI 联调; 时间线(INTERFACE.md §4):
 *   joining 3s → joined | 发现在 Start_discovery 内阻塞 2s
 *   preparing 2s → sending(0→40, 2%/s) → printing(40→100, 2%/s) → done 2s → idle
 * 彩蛋(便于测错误路径): psk 1..7 位 → 同步 failed(空串=开放网络§20); ssid 含 "fail" → 异步失败回 disconnected
 * §18 预览/裁切三方法: mock 无图像管线(libjpeg 不链接), 明确回 failed;
 *   需要真实图像行为用 --real 后端(wifi_backend=mock 即可, 打印侧走 mock_ipp)。
 */
#include "backend.h"
#include "state.h"
#include "log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- 连接模拟 ---- */
static int  g_joining;            /* 1=joining 进行中 */
static int  g_join_fail;          /* 异步失败演示 */
static long long g_join_t0;

/* ---- 打印任务模拟 ---- */
enum { MJ_NONE = 0, MJ_PREPARING, MJ_SENDING, MJ_PRINTING, MJ_DONE, MJ_CANCELED };
static int      g_mj = MJ_NONE;
static long long g_mj_t0;
static int      g_jobno;

/* ---- 发现模拟(v2.2 语义: 受理即返, tick 2s 后发布 —— 与真后端同节奏) ---- */
static int      g_disc_run;
static long long g_disc_t0;

/* 字符串数组工具(元素 malloc, 由调用方逐个 free) */
static char **arr_new(int n) { return calloc((size_t)n + 1, sizeof(char *)); }
static void arr_put(char ***out, int *n, char **items, int cnt)
{
    *out = items; *n = cnt;
}

static int mb_connect_to_ap(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return PRN_BE_PARAM;
    /* §20: psk 空串=开放网络(免密); 1..7 位=半截密码同步失败 */
    if (!psk || (psk[0] && strlen(psk) < 8)) {
        LOGW("mock: psk invalid (%zu, empty=open)", psk ? strlen(psk) : 0);
        return PRN_BE_FAILED;
    }
    if (!prn_state_is("disconnected")) return PRN_BE_BUSY;

    g_joining = 1;
    g_join_fail = (strstr(ssid, "fail") != NULL);
    g_join_t0 = now_ms();
    prn_set_string(PRN_SSID, ssid);          /* 顺手记住目标 ssid */
    prn_set_connection_state("joining");     /* 异步 3s 后 → joined/disconnected(tick) */
    LOGI("mock: joining '%s' (3s)", ssid);
    return 0;
}

/* §20 扫描选择: 模拟 2.5s 扫描, 4 个假网络(含打印机热点样式 hannto-* 与一个 open),
 * 最强(-50)置顶; busy 语义=非 disconnected 态(joining/joined/ready/discovering)全回 busy */
static int mb_scan_wifi(char ***out, int *n)
{
    if (!prn_state_is("disconnected")) {
        LOGW("mock: scan_wifi in state %s → busy", prn_state_get()->connection_state);
        return PRN_BE_BUSY;
    }
    /* 阻塞模拟扫描(真后端=iw/wpa_cli 扫描窗口) */
    for (int i = 0; i < 25; i++) usleep(100 * 1000);

    static const char *const nets[] = {
        "hannto-printer-honey1s_mibt4D0F\t-50\twpa2",  /* 打印机热点(-50 最强置顶) */
        "guguji-IOT\t-58\twpa2",
        "Coffee-Guest\t-72\topen",                       /* 开放网络: 免密码直连 */
        "HomeNet_5G\t-80\twpa2",
    };
    prn_scanned_clear();
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
        prn_scanned_add(nets[i]);
    prn_state *st = prn_state_get();
    char **items = arr_new(st->scanned.n);
    for (int i = 0; i < st->scanned.n; i++)
        items[i] = strdup(st->scanned.items[i]);
    arr_put(out, n, items, st->scanned.n);
    LOGI("mock: scan_wifi done, %d network(s) (2.5s)", st->scanned.n);
    return 0;
}

static int mb_disconnect_restore(void)
{
    g_joining = 0;
    if (prn_job_active()) {                  /* 拆链=强取消 */
        g_mj = MJ_NONE;
        prn_set_job_status("canceled");
        prn_set_job_progress(0);
        prn_set_printer_state("idle");
    }
    prn_discovered_clear();
    prn_set_printer_name("");
    prn_set_printer_ip("");
    prn_set_connection_state("disconnected");
    LOGI("mock: disconnected, wifi restored");
    return 0;
}

static int mb_start_discovery(char ***out, int *n)
{
    *out = NULL;
    *n = 0;
    if (prn_state_is("joining"))   return PRN_BE_BUSY;
    if (!prn_state_is("joined"))   return PRN_BE_FAILED;

    prn_set_connection_state("discovering");
    /* v2.2 语义(与真后端一致): 受理即返空 as, 模拟扫描 2s 在 tick 内完成,
     * 结果经 discovered_printers notify 发布 */
    g_disc_run = 1;
    g_disc_t0 = now_ms();
    LOGI("mock: discovery 受理(2s 后经 discovered_printers notify 发布)");
    return 0;
}

static int mb_select_printer(const char *name)
{
    prn_state *st = prn_state_get();
    if (st->discovered_n == 0) { LOGW("mock: select before discovery"); return PRN_BE_FAILED; }
    /* 先匹配列表(未命中=parametererror), 状态门在后(重选允许) */
    for (int i = 0; i < st->discovered_n; i++) {
        const char *el = st->discovered[i];
        const char *tab = strchr(el, '\t');
        size_t nlen = tab ? (size_t)(tab - el) : strlen(el);
        int full = !strcmp(el, name);                       /* 整串匹配 */
        int part = (tab && strlen(name) == nlen && !strncmp(el, name, nlen)); /* 纯名字 */
        if (full || part) {
            if (!prn_state_is("joined") && !prn_state_is("ready")) {
                LOGW("mock: select in state %s", st->connection_state);
                return PRN_BE_FAILED;
            }
            char nm[PRN_SLEN], ip[PRN_SLEN];
            snprintf(nm, sizeof nm, "%.*s", (int)nlen, el);
            snprintf(ip, sizeof ip, "%s", tab ? tab + 1 : "");
            prn_set_printer_name(nm);
            prn_set_printer_ip(ip);
            prn_set_connection_state("ready");
            LOGI("mock: selected '%s' @ %s", nm, ip);
            return 0;
        }
    }
    LOGW("mock: '%s' not in discovered list", name);
    return PRN_BE_PARAM;
}

static int suffix_is(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

static int mb_photo_list(const char *path, char ***out, int *n)
{
    if (!path || !*path) return PRN_BE_PARAM;
    DIR *d = opendir(path);
    if (!d) { LOGW("mock: opendir '%s' failed", path); return PRN_BE_PARAM; }
    char **items = arr_new(64);
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && cnt < 64) {
        if (e->d_name[0] == '.') continue;
        if (suffix_is(e->d_name, ".jpg") || suffix_is(e->d_name, ".jpeg") ||
            suffix_is(e->d_name, ".png")) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s/%s", path, e->d_name);
            items[cnt++] = strdup(buf);
        }
    }
    closedir(d);
    arr_put(out, n, items, cnt);
    LOGI("mock: photo_list '%s' -> %d", path, cnt);
    return 0;
}

static int mb_start_print_job(char **paths, int npaths, const prn_job_opts_t *opts,
                              char **out_job_id)
{
    (void)paths; (void)opts;                          /* mock 不做能力校验/图像处理 */
    *out_job_id = NULL;
    if (!prn_state_is("ready")) { LOGW("mock: print in state %s", prn_state_get()->connection_state); return PRN_BE_FAILED; }
    if (prn_job_active()) return PRN_BE_BUSY;

    g_mj = MJ_PREPARING;
    g_mj_t0 = now_ms();
    prn_set_job_status("preparing");
    prn_set_printer_state("busy");
    char id[32];
    snprintf(id, sizeof id, "job-%d", ++g_jobno);
    *out_job_id = strdup(id);
    LOGI("mock: print job %s (%d file(s))", id, npaths);
    return 0;
}

/* 后端失败详情: mock 永不填(backend.h 声明带 weak → 本定义为弱符号,
 * 与 backend_real 同链时强定义覆盖, 仅链接 mock 时生效) */
const char *prn_be_last_error(void) { return ""; }

/* §18 三方法: mock 无图像管线, 回 failed(真行为用 --real 后端) */
static int mb_photo_info(const char *path, prn_photo_info_t *out)
{
    (void)path; (void)out;
    LOGW("mock: photo_info 无图像管线(用 --real 后端)");
    return PRN_BE_FAILED;
}
static int mb_photo_preview(const char *path, int max_w, int max_h, char **out_path)
{
    (void)path; (void)max_w; (void)max_h; (void)out_path;
    LOGW("mock: photo_preview 无图像管线(用 --real 后端)");
    return PRN_BE_FAILED;
}
static int mb_preview_print(const char *path, const prn_job_opts_t *opts,
                            char **out_path)
{
    (void)path; (void)opts; (void)out_path;
    LOGW("mock: preview_print 无图像管线(用 --real 后端)");
    return PRN_BE_FAILED;
}

static int mb_cancel_job(void)
{
    if (!prn_job_active()) return 0;
    g_mj = MJ_CANCELED;
    g_mj_t0 = now_ms();
    prn_set_job_status("canceled");
    LOGI("mock: job canceled");
    return 1;
}

static void mb_tick(void)
{
    /* 发现状态机推进(v2.2 受理即返: 2s 后发布 discovered_printers + 回 joined) */
    if (g_disc_run) {
        if (now_ms() - g_disc_t0 >= 2000) {
            g_disc_run = 0;
            prn_discovered_clear();               /* 模拟两台(元素 "名字\tIP") */
            prn_discovered_add("Mi Photo Printer 1S [A1B2]\t192.168.43.1");
            prn_discovered_add("DIRECT-8B-Mi-Photo-Printer1S\t192.168.49.1");
            prn_set_connection_state("joined");
            LOGI("mock: discovery done, %d found", prn_state_get()->discovered_n);
        }
        return;                                    /* 发现期间不推进任务 */
    }

    /* 连接状态机推进 */
    if (g_joining) {
        if (now_ms() - g_join_t0 >= 3000) {
            g_joining = 0;
            if (g_join_fail) {
                prn_set_connection_state("disconnected");
                LOGW("mock: join failed (ssid contains 'fail')");
            } else {
                prn_set_connection_state("joined");
                LOGI("mock: joined");
            }
        }
        return;                                /* 连接期间不推进任务 */
    }

    /* 打印任务状态机推进(2%/秒) */
    prn_state *st = prn_state_get();
    long long dt = now_ms() - g_mj_t0;
    switch (g_mj) {
    case MJ_PREPARING:
        if (dt >= 2000) { g_mj = MJ_SENDING; g_mj_t0 = now_ms(); prn_set_job_status("sending"); }
        break;
    case MJ_SENDING:
        prn_set_job_progress((int)(dt / 500));            /* 2%/s */
        if (st->job_progress >= 40) { g_mj = MJ_PRINTING; g_mj_t0 = now_ms(); prn_set_job_status("printing"); }
        break;
    case MJ_PRINTING:
        prn_set_job_progress(40 + (int)(dt / 500));
        if (st->job_progress >= 100) { g_mj = MJ_DONE; g_mj_t0 = now_ms(); prn_set_job_progress(100); prn_set_job_status("done"); }
        break;
    case MJ_DONE:
        if (dt >= 2000) { g_mj = MJ_NONE; prn_set_job_status("idle"); prn_set_job_progress(0); prn_set_printer_state("idle"); }
        break;
    case MJ_CANCELED:
        if (dt >= 1000) { g_mj = MJ_NONE; prn_set_job_status("idle"); prn_set_job_progress(0); prn_set_printer_state("idle"); }
        break;
    case MJ_NONE:
    default:
        break;
    }
}

static const prn_backend g_mb = {
    .name = "mock",
    .tick = mb_tick,
    .connect_to_ap = mb_connect_to_ap,
    .scan_wifi = mb_scan_wifi,                        /* §20 扫描选择 */
    .disconnect_restore = mb_disconnect_restore,
    .start_discovery = mb_start_discovery,
    .select_printer = mb_select_printer,
    .photo_list = mb_photo_list,
    .start_print_job = mb_start_print_job,
    .cancel_job = mb_cancel_job,
    .photo_info = mb_photo_info,                     /* §18: stub */
    .photo_preview = mb_photo_preview,               /* §18: stub */
    .preview_print = mb_preview_print,               /* §18: stub */
};

const prn_backend *prn_backend_mock(void) { return &g_mb; }
