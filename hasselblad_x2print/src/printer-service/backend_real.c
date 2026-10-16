/* backend_real.c — 真打印后端(通用 IPP, 能力一切自探测, 零硬编码打印机属性)
 *
 * 组合已验证的打印核心:
 *   src/net/ipp.c    IPP 客户端(Get-Printer-Attributes[all+media-col-database] /
 *                    Print-Job / 轮询)
 *   src/net/mdns.c   mDNS/DNS-SD 发现(_ipp._tcp + _universal._sub._ipp._tcp
 *                    AirPrint 子类型, TXT rp= 提取端点路径)
 *   src/img/imgprep.c EXIF 旋转→居中裁切→缩放→JPEG 重编码
 *   wifi_cam.c       相机 Wi-Fi STA 编排(阶段3真机验证; 联调走 wifi_backend=mock)
 *
 * v2 通用化(INTERFACE.md §2b):
 *   - Select_printer 成功后立即能力探测: 格式协商(image/jpeg 直发优先, 仅
 *     栅格格式→failed"该打印机需栅格后端, 暂不支持"), 全量能力经
 *     prn_caps_publish 发布(supported 系列/copies_max/caps_ready/media_size_info)
 *   - 打印选项=打印机原生 IPP 值: media/print_quality/color_mode 字符串直传,
 *     copies ≤ copies_max; 越能力值 parametererror(Set 与 Start_print_job 双闸)
 *   - 目标尺寸自计算: 选中 media 物理尺寸(media-col x/y-dimension µm) ×
 *     printer-resolution-default(缺失兜底 300dpi) = imgprep 目标像素; 横竖
 *     跟随内容(EXIF 旋转后) 与纸张短边匹配
 *   - 发现: mDNS 同时查 _ipp._tcp 与 _universal._sub._ipp._tcp, TXT rp=
 *     非 /ipp/print 也支持(ipp_set_path); 直连兜底 printer_ip 支持逗号分隔
 *     多端点 host[:port](WSL 彩排: 多台 mock 共环回口)
 *
 * §18 自由裁切/旋转/预览(INTERFACE.md v2.1):
 *   - Start_print_job opts 扩展 crop_x/y/w/h(d)+rotation(i), 受理时定形进
 *     g_rj.edit, rj_prep_file 统一走 imgprep_prepare_edit;
 *   - Get_photo_info(imgprep_probe 头部探测)/Get_photo_preview(FAST+缓存,
 *     3FR 感知)/Preview_print(WYSIWYG: 与打印同管线同参数, rj_prep_file 共用
 *     ⇒ 预览字节=打印发出的字节)。
 */
#include "netlog.h"      /* 先 netlog(取钩子原型与级别枚举), 撤其 LOGx 宏换服务端日志 */
#undef LOGD
#undef LOGI
#undef LOGW
#undef LOGE

#include "backend.h"
#include "adaptor.h"     /* prn_adaptor_emit_rx: rx(y,ay) 事件帧发射点(odindb §7b) */
#include "state.h"
#include "config.h"
#include "log.h"
#include "wifi_cam.h"
#include "ipp.h"
#include "mdns.h"
#include "imgprep.h"
#include "3fr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>       /* inet_pton(直连 IP 选中判定) */
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#if defined(__ANDROID__)
#define X2P_TMPDIR "/data/x2print/tmp"   /* 相机: /tmp 不保证存在 */
#define X2P_ROOT   "/data/x2print"
#else
#define X2P_TMPDIR "/tmp/x2print"
#define X2P_ROOT   "/tmp/x2print"
#endif

/* ---------------- netlog → 服务日志桥(级别序一致: 0 debug .. 3 error) ---------------- */
static void netlog_bridge(x2p_log_level_t lvl, const char *fmt, va_list ap)
{
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (lvl <= X2P_LOG_DEBUG)      LOGD("[net] %s", buf);
    else if (lvl == X2P_LOG_INFO)  LOGI("[net] %s", buf);
    else if (lvl == X2P_LOG_WARN)  LOGW("[net] %s", buf);
    else                           LOGE("[net] %s", buf);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 后端失败详情(错误回复正文带出) ---------------- */
static char g_be_err[192];
static void be_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_be_err, sizeof g_be_err, fmt, ap);
    va_end(ap);
}
const char *prn_be_last_error(void) { return g_be_err; } /* 强定义(mock 侧弱定义) */

/* ---------------- 目标打印机缓存 ---------------- */
static char g_ip[64];       /* 当前选中打印机 IP(Select_printer 时填) */
static int  g_port;         /* IPP 端口(0=631) */
static ipp_caps_t g_caps;   /* 选中打印机能力(Select_printer 探测; 打印参数之源) */

/* 发现端点侧表: discovered 元素只含 "名字\tIP"(线协议契约), 端口/路径挂这里 */
typedef struct {
    char name[PRN_SLEN];
    char ip[64];
    int  port;              /* 0=默认 631 */
    char path[96];          /* mDNS TXT rp=(已含 /); 空=默认 /ipp/print */
} prn_ep_t;
static prn_ep_t g_eps[PRN_MAX_PRINTERS];
static int g_eps_n;

static void eps_clear(void) { g_eps_n = 0; }

/* 登记端点(同 IP 不同端口的多台 mock/同 IP 双栈均可区分); 返回下标或 -1 */
static int eps_add(const char *name, const char *ip, int port, const char *path)
{
    if (!name || !ip || g_eps_n >= PRN_MAX_PRINTERS) return -1;
    prn_ep_t *e = &g_eps[g_eps_n];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->ip, sizeof e->ip, "%s", ip);
    e->port = port > 0 ? port : 0;
    snprintf(e->path, sizeof e->path, "%s", path && path[0] ? path : "");
    return g_eps_n++;
}

/* 按选中参数定位端点: 接受 "名字\tIP" 整串 / 纯名字 / 纯 IP(重复 IP 取首个) */
static const prn_ep_t *eps_find(const char *name, char *name_out, size_t name_cap)
{
    for (int i = 0; i < g_eps_n; i++) {
        char full[PRN_SLEN + 80];
        snprintf(full, sizeof full, "%.95s\t%.63s", g_eps[i].name, g_eps[i].ip);
        if (!strcmp(full, name) || !strcmp(g_eps[i].name, name) ||
            !strcmp(g_eps[i].ip, name)) {
            snprintf(name_out, name_cap, "%.95s", g_eps[i].name);
            return &g_eps[i];
        }
    }
    return NULL;
}

static int cfg_port(void)
{
    const char *p = prn_config_get("ipp_port");
    int v = p[0] ? atoi(p) : 0;
    return (v > 0 && v < 65536) ? v : 0;
}

static int wifi_is_mock(void)
{
    return !strcmp(prn_config_get("wifi_backend"), "mock");
}

/* ---------------- 探测子进程运行器(P0 反饿死: 网络探测全部 fork+poll) ----------------
 * 发现的每端点 IPP 验明 / mDNS 窗口 / /24 扫描 / Cancel 确认环都在子进程里
 * 阻塞, 父进程 tick 内 waitpid(WNOHANG) 轮询 + 预算 SIGKILL 兜底 —— 主循环
 * 任何步骤都不被网络挂起(黑洞端点实测: Get 应答 ≤2s, NOTES §22)。
 * 子进程经 pipe 写回短文本结果(行式), 收尸后一次 read 取尽(写入量 << 64K
 * 管道缓冲, 子退出即 EOF, read 不阻塞)。子内静音网络层日志、不碰 D-Bus。 */
static const char *txt_rp(const char *txt);                    /* 定义在后 */
static int tcp_scan_subnet(struct in_addr base, int port, in_addr_t *hits, int max_hits);

static struct {
    pid_t     pid;
    int       fd;                 /* 结果管道读端 */
    long long t0;
    int       budget_ms;
    const char *what;             /* 日志用 */
} g_ch;

static int child_run(const char *what, int budget_ms, void (*fn)(void *), void *arg)
{
    if (g_ch.pid > 0) {
        LOGW("real: 探测子进程仍在跑(%s), 拒绝并发", g_ch.what);
        return -1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) { LOGE("real: pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); LOGE("real: fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, STDERR_FILENO);
        x2p_netlog_set_hook(NULL);              /* 子内静音 ipp.c 日志 */
        fn(arg);
        fflush(stdout);
        _exit(0);
    }
    close(pfd[1]);
    g_ch.pid = pid;
    g_ch.fd = pfd[0];
    g_ch.t0 = now_ms();
    g_ch.budget_ms = budget_ms;
    g_ch.what = what;
    return 0;
}

/* tick 轮询: 1=在跑 0=结束(结果文本入 out; 超时被杀=空结果)。非阻塞。 */
static int child_poll(char *out, size_t cap)
{
    out[0] = 0;
    if (g_ch.pid <= 0) return 0;
    int st = 0;
    pid_t r = waitpid(g_ch.pid, &st, WNOHANG);
    if (r == 0) {
        if (now_ms() - g_ch.t0 <= g_ch.budget_ms) return 1;
        LOGW("real: %s 超时(%dms), SIGKILL(黑洞端点快速放弃)", g_ch.what, g_ch.budget_ms);
        kill(g_ch.pid, SIGKILL);
        waitpid(g_ch.pid, &st, 0);
        close(g_ch.fd);
        g_ch.pid = 0;
        return 0;
    }
    g_ch.pid = 0;
    if (r > 0) {
        ssize_t n = read(g_ch.fd, out, cap - 1);
        if (n < 0) n = 0;
        out[n] = 0;
    }
    close(g_ch.fd);
    return 0;
}

/* 中断(Disconnect/Cancel): 立即杀在途探测子进程 */
static void child_abort(void)
{
    if (g_ch.pid <= 0) return;
    LOGW("real: 探测子进程中断(%s), SIGKILL", g_ch.what);
    kill(g_ch.pid, SIGKILL);
    int st;
    waitpid(g_ch.pid, &st, 0);
    close(g_ch.fd);
    g_ch.pid = 0;
}

/* ---- 子进程体: 单端点 IPP 验明(Get-Printer-Attributes) ---- */
typedef struct { char ip[64]; int port; char path[96]; } probe_arg_t;
static void probe_child(void *p)
{
    probe_arg_t *a = (probe_arg_t *)p;
    ipp_set_path(a->path[0] ? a->path : NULL);
    ipp_set_timeouts(5000, 12000);              /* 冷启动容忍档: 闲置打印机首个 GPA 实测可超 5s
                                                   (2026-09-26 真机: 紧档 3s/5s 误杀); 黑洞端点
                                                   仍有父进程 DISC_PROBE_MS kill 兜底 */
    ipp_caps_t caps;
    if (ipp_get_printer_attributes(a->ip, a->port, &caps) != IPP_OK) {
        printf("FAIL\n");
        return;
    }
    printf("OK\t%.95s\n", caps.printer_name[0] ? caps.printer_name : "IPP Printer");
}

/* ---- 子进程体: mDNS 窗口(_ipp._tcp + AirPrint 子类型, 6s) ---- */
static void mdns_child(void *p)
{
    (void)p;
    mdns_service_t svcs[8];
    int c = mdns_discover_ipp_if(svcs, 8, 6000, "wlan0");
    for (int i = 0; i < c && i < 8; i++) {
        if (!svcs[i].ip[0]) continue;
        printf("SVC\t%.63s\t%d\t%d\t%.159s\t%.700s\n", svcs[i].ip, svcs[i].port,
               svcs[i].universal, svcs[i].instance, svcs[i].txt);
    }
}

/* ---- 子进程体: /24 端口快扫(结果 IP 行) ---- */
typedef struct { struct in_addr base; int port; } scan_arg_t;
static void netscan_child(void *p)
{
    scan_arg_t *a = (scan_arg_t *)p;
    in_addr_t hits[8];
    int n = tcp_scan_subnet(a->base, a->port, hits, 8);
    for (int i = 0; i < n; i++) {
        struct in_addr h = { .s_addr = hits[i] };
        printf("HIT\t%s\n", inet_ntoa(h));
    }
}

/* ---- 子进程体: 单次 Get-Job-Attributes(打印轮询, 主循环去阻塞) ---- */
typedef struct { char ip[64]; int port; int id; } poll_arg_t;
static void poll_child(void *p)
{
    poll_arg_t *a = (poll_arg_t *)p;
    ipp_set_timeouts(4000, 10000);              /* 忙碌打印机容忍档(§23) */
    ipp_job_t job;
    if (ipp_get_job_attributes(a->ip, a->port, a->id, &job) != IPP_OK) return;
    printf("ST\t%d\n", job.job_state);
}

/* ---- 子进程体: Cancel-Job 下发 + 有界确认环(≤3s, 250ms 步进) ----
 * 整环在子进程内(P0: Cancel_job 方法即返, 确认不再叠加在主循环上);
 * 定义在任务状态机之后(RJ_MAX_FILES)。见 g_rj 下方 cancel_child。 */

/* ---------------- JPEG 头嗅探: 存储尺寸 + EXIF Orientation(不解码像素) ----------------
 * 只读 SOFn 与 APP1/Exif Orientation; 判定与 imgprep 内部一致。 */
static int jpeg_scan_size(const char *path, int *w_out, int *h_out, int *orient_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char soi[2];
    if (fread(soi, 1, 2, f) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) { fclose(f); return -1; }

    int w = 0, h = 0, orient = 1;
    for (;;) {
        int m = fgetc(f);
        if (m == EOF) break;
        if (m != 0xFF) continue;                       /* 段间垃圾: 跳过找标记 */
        while (m == 0xFF) m = fgetc(f);                /* 0xFF 填充 */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) continue;
        unsigned char lb[2];
        if (fread(lb, 1, 2, f) != 2) break;
        int len = (lb[0] << 8) | lb[1];                /* 段长含自身 2 字节 */
        if (len < 2) break;
        if (m == 0xDA) break;                          /* SOS: 之后是熵流 */

        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            unsigned char sof[6];                      /* precision,h(2),w(2) */
            if (len < 8 || fread(sof, 1, 6, f) != 6) break;
            h = (sof[1] << 8) | sof[2];
            w = (sof[3] << 8) | sof[4];
        } else if (m == 0xE1 && len >= 10) {           /* APP1: Exif Orientation */
            size_t blen = (size_t)len - 2;
            unsigned char *buf = malloc(blen);
            if (buf && fread(buf, 1, blen, f) == blen && blen >= 12 &&
                !memcmp(buf, "Exif\0\0", 6)) {
                const unsigned char *t = buf + 6;      /* TIFF 头(解法同 imgprep) */
                unsigned tlen = (unsigned)blen - 6;
                int be = (t[0] == 'M' && t[1] == 'M');
                #define RD16(p) (be ? ((p)[0] << 8) | (p)[1] : ((p)[1] << 8) | (p)[0])
                #define RD32(p) (be ? (RD16(p) << 16) | RD16((p) + 2) \
                                  : (RD16((p) + 2) << 16) | RD16(p))
                if (tlen >= 8 && RD16(t + 2) == 42) {
                    unsigned off = (unsigned)RD32(t + 4);
                    if (off + 2 <= tlen) {
                        int n = RD16(t + off);         /* IFD0 条目数 */
                        for (int i = 0; i < n; i++) {
                            unsigned ep = off + 2 + (unsigned)i * 12;
                            if (ep + 12 > tlen) break;
                            const unsigned char *e = t + ep;
                            if (RD16(e) == 0x0112 && RD16(e + 2) == 3) {
                                int v = RD16(e + 8);   /* Orientation(SHORT 内联) */
                                if (v >= 1 && v <= 8) orient = v;
                                break;
                            }
                        }
                    }
                }
                #undef RD16
                #undef RD32
            }
            free(buf);
        } else {
            if (fseek(f, len - 2, SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    if (!w || !h) return -1;
    *w_out = w;
    *h_out = h;
    *orient_out = orient;
    return 0;
}

/* §18: 旋转后逻辑尺寸 —— 显式 rotation(0/90/180/270) **覆盖** EXIF(以存储
 * 像素为基准重算), -1/无 edit = EXIF 自动。3FR 传提取出的 preview JPEG 路径。 */
static int oriented_size_ex(const char *path, const imgprep_edit_t *e, int *ow, int *oh)
{
    int w, h, o;
    if (jpeg_scan_size(path, &w, &h, &o) != 0) return -1;
    int swap;
    if (e && e->rotation >= 0) swap = ((e->rotation / 90) % 2) == 1;
    else                       swap = (o >= 5);
    *ow = swap ? h : w;
    *oh = swap ? w : h;
    return 0;
}

/* 目标像素按纸张横竖自适应: 内容(旋转后, 含 §18 显式旋转)横版↔纸张横版对齐,
 * 方纸不换 */
static void target_for_file(const char *path, int pw, int ph,
                            const imgprep_edit_t *edit, int *tw, int *th)
{
    *tw = pw;
    *th = ph;
    if (pw == ph) return;                         /* 方形纸: 无所谓横竖 */
    int w, h;
    if (oriented_size_ex(path, edit, &w, &h) == 0 &&
        ((w > h) != (pw > ph))) {                 /* 内容与纸张横竖不一致: 交换 */
        *tw = ph;
        *th = pw;
    }
}

/* ---------------- 打印任务状态机(tick 推进) ---------------- */
static int suffix_is(const char *s, const char *suf);   /* 前置(定义在 photo_list 前) */
enum { RJ_IDLE = 0, RJ_PREP, RJ_SEND, RJ_PRINT, RJ_DONE, RJ_FAILED, RJ_CANCELED };
#define RJ_MAX_FILES 64
#define RJ_POLL_MS        1000
#define RJ_TOTAL_TIMEOUT_MS_BASE   120000              /* 打印阶段总超时: 基础 2min */
#define RJ_TOTAL_TIMEOUT_MS_PAGE   75000               /* + 75s/页(染料热升华 ~58s/张 + 余量) */
#define RJ_HOLD_MS        3000     /* 终态保持后回 idle */

static struct {
    int  phase;
    char *files[RJ_MAX_FILES];                        /* strdup 路径 */
    int  nfiles, cur;
    int  copies;
    int  jobno;                                       /* 服务级 job 序号(job-N) */
    int  ipp_ids[RJ_MAX_FILES];                       /* 每文件一个 IPP job-id */
    int  done_jobs;
    long long t0, last_poll;
    int  poll_armed;                                 /* 轮询子进程在途 */
    char tmps[RJ_MAX_FILES][128];                     /* 每文件一个 imgprep 中间产物 */
    /* 受理时定形的生效打印参数(v2: 原生 IPP 值) */
    int  paper_w, paper_h;                            /* 纸张基准像素(media 尺寸×分辨率) */
    int  quality_enum;                                /* print-quality 枚举(0=不发) */
    char media[PRN_SLEN];                             /* 原生 media 关键字 */
    char color[PRN_SLEN];                             /* 原生 print-color-mode */
    char scaling[32];                                 /* print-scaling(空=不发) */
    imgprep_edit_t edit;                              /* §18 裁切/旋转(受理时定形) */
} g_rj;

/* ---- 取消环 rx 事件帧类型(定义提前: 取消子进程区先于 rx 帧区使用) ----
 * 帧格式见下方 rj_emit_rx 注释。 */
enum { RX_EV_JOB_STATUS = 1, RX_EV_CANCEL = 2 };

/* ---- Cancel-Job 下发+确认环子进程体(≤3s 确认环在子进程内, NOTES §22 P0) ---- */
static void rj_emit_rx(int ev_type, unsigned job_id, const char *fmt, ...);  /* 定义在后 */
typedef struct { char ip[64]; int port; int ids[RJ_MAX_FILES]; int n; } cancel_arg_t;
static cancel_arg_t g_cancel_arg;
static int g_cancel_pending;              /* 1=取消环子进程在途(tick 收尾发 rx 帧) */

static void cancel_child(void *p)
{
    cancel_arg_t *a = (cancel_arg_t *)p;
    ipp_set_timeouts(4000, 10000);              /* 取消容忍档: 打印机忙碌期间应答慢(2026-09-26
                                                   真机 sent=0 实证——快档 2s 全超时, 取消形同虚设) */
    int sent = 0;
    for (int i = 0; i < a->n; i++)
        if (ipp_cancel_job(a->ip, a->port, a->ids[i]) == IPP_OK) sent++;
    int confirmed = 0;
    for (int try = 0; try < 12 && sent; try++) {
        int all = 1;
        for (int i = 0; i < a->n; i++) {
            ipp_job_t job;
            if (ipp_get_job_attributes(a->ip, a->port, a->ids[i], &job) == IPP_OK &&
                job.job_state != 7)
                all = 0;
        }
        if (all) { confirmed = sent; break; }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 250L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    printf("RES\t%d\t%d\n", sent, confirmed);
}

/* 本地取消 + IPP 取消子进程受理(Cancel_job / Disconnect 强取消共用):
 * 本地状态立即转 canceled(方法即返), IPP Cancel-Job 下发+确认在子进程,
 * tick_cancel() 收尸后发 rx(ev=2) 帧与日志。 */
static void cancel_job_async(void)
{
    /* 抢占在途子进程(打印轮询): 单 g_ch 槽位, 不杀则 cancel spawn 失败(§23) */
    if (g_ch.pid > 0) {
        kill(g_ch.pid, SIGKILL);
        int st; waitpid(g_ch.pid, &st, 0);
        close(g_ch.fd);
        g_ch.pid = 0;
        g_rj.poll_armed = 0;
    }
    int n = 0;
    g_cancel_arg.ip[0] = 0;
    snprintf(g_cancel_arg.ip, sizeof g_cancel_arg.ip, "%.63s", g_ip);
    g_cancel_arg.port = g_port;
    for (int i = 0; i < g_rj.nfiles && n < RJ_MAX_FILES; i++)
        if (g_rj.ipp_ids[i] > 0) g_cancel_arg.ids[n++] = g_rj.ipp_ids[i];
    g_cancel_arg.n = n;
    if (n > 0 && g_ip[0]) {
        if (child_run("ipp cancel", 30000, cancel_child, &g_cancel_arg) == 0)
            g_cancel_pending = 1;
        else
            LOGW("real: 取消环子进程 spawn 失败 —— 仅本地取消(远端任务或继续)");
    }
}

static void tick_cancel(void)
{
    if (!g_cancel_pending) return;
    char out[64];
    if (child_poll(out, sizeof out) == 1) return;      /* 确认环仍在跑 */
    g_cancel_pending = 0;
    int sent = 0, confirmed = 0;
    sscanf(out, "RES\t%d\t%d", &sent, &confirmed);
    if (sent && !confirmed)
        LOGW("real: Cancel-Job 已下发 %d 个但 3s 内未确认 canceled", sent);
    rj_emit_rx(RX_EV_CANCEL, (unsigned)g_rj.jobno, "ipp_cancel sent=%d confirmed=%d",
               sent, confirmed);
    LOGI("real: job canceled (IPP Cancel-Job: sent=%d confirmed=%d)", sent, confirmed);
}

static void rj_free_files(void)
{
    for (int i = 0; i < g_rj.nfiles; i++) {
        free(g_rj.files[i]);
        g_rj.files[i] = NULL;
        if (g_rj.tmps[i][0]) {
            remove(g_rj.tmps[i]);                     /* 清理中间产物 */
            g_rj.tmps[i][0] = 0;
        }
    }
    g_rj.nfiles = 0;
    g_rj.cur = 0;
}

/* opts → imgprep 编辑参数(§18; 定义在后, 此处前置供 rb_start_print_job 用) */
static void opts_to_edit(const prn_job_opts_t *o, imgprep_edit_t *e);

/* ---------------- rx 事件帧(odindb 兼容信号 rx(y,ay) 的载荷协议) ----------------
 * 打印任务生命周期事件除 propertiesChanged 外, 同时以 rx(y,ay) 广播一帧紧凑
 * 事件(供 odindb-send-printer --signal rx / GUI 调试面订阅)。字段一律大端:
 *   偏移 0   3 B  magic "X2P"(0x58 'X', 0x32 '2', 0x50 'P') —— 明文可辨
 *   偏移 3   2 B  ev_type(u16): 1=job_status 变更 2=Cancel-Job 下发结果
 *   偏移 5   4 B  job_id(u32): 服务级任务号("job-N" 的 N; 无关事件=0)
 *   偏移 9   ..   文本摘要(ASCII, 无 NUL 终止, 截断到帧尾, ≤246 B)
 * 帧总长 ≤ 255 B(rx 的 y message_len 为 u8); y message_len = 帧总长。
 * ev=1 摘要形如 "job_status=printing" / "job_status=failed; ipp print";
 * ev=2 摘要形如 "ipp_cancel sent=1 confirmed=1"(下发/确认的 IPP job 数)。
 * 另一路 rx 来源是 tx() 的 echo 回显(adaptor.c m_tx, 原样字节, 非 X2P 帧)。 */
static void rj_emit_rx(int ev_type, unsigned job_id, const char *fmt, ...)
{
    unsigned char f[255];
    va_list ap;
    f[0] = 'X'; f[1] = '2'; f[2] = 'P';
    f[3] = (unsigned char)((unsigned)ev_type >> 8);
    f[4] = (unsigned char)((unsigned)ev_type & 0xff);
    f[5] = (unsigned char)(job_id >> 24);
    f[6] = (unsigned char)(job_id >> 16);
    f[7] = (unsigned char)(job_id >> 8);
    f[8] = (unsigned char)job_id;
    va_start(ap, fmt);
    int want = vsnprintf((char *)f + 9, sizeof f - 9, fmt, ap);
    va_end(ap);
    int sl = want < 0 ? 0 : (want > (int)(sizeof f - 9 - 1) ? (int)(sizeof f - 9 - 1) : want);
    int n = 9 + sl;
    prn_adaptor_emit_rx((unsigned char)n, f, n);
    LOGD("real: rx event ev=%d job=%u n=%d [%.*s]", ev_type, job_id, n, sl, (const char *)f + 9);
}

/* job_status 变更统一入口: propertiesChanged(notify 链) + rx 事件帧(ev=1) 双发 */
static void rj_set_status(const char *status, const char *why)
{
    prn_set_job_status(status);
    if (why) rj_emit_rx(RX_EV_JOB_STATUS, (unsigned)g_rj.jobno, "job_status=%s; %s", status, why);
    else     rj_emit_rx(RX_EV_JOB_STATUS, (unsigned)g_rj.jobno, "job_status=%s", status);
}

/* ---------------- 单文件打印准备(§18 抽出共享: 打印任务与 WYSIWYG 预览同管线) ----------------
 * 输入分流(§16): .3fr/.3FR → h3fr 提取内嵌 preview 落临时 JPEG(方向判定用)
 * → 按内容横竖(含 edit 显式旋转)自适应目标 → imgprep HQ(3FR q95/其余 q92)。
 * tick_job(RJ_PREP) 与 rb_preview_print 共用本函数 —— 保证 doPreview_print
 * 输出与打印发出的 JPEG 逐字节一致(同参数同管线, 编码器确定性)。
 * 返回 0=ok(非 0 失败原因已 LOGE/be_err)。 */
static int rj_prep_file(const char *cur, const char *dst, int paper_w, int paper_h,
                        const imgprep_edit_t *edit, imgprep_report_t *rep)
{
    char src_path[192];
    char prev_tmp[160];
    int tw, th, e, is_3fr = suffix_is(cur, ".3fr");

    snprintf(src_path, sizeof src_path, "%s", cur);
    prev_tmp[0] = 0;
    if (is_3fr) {
        unsigned char *prev = NULL;
        size_t plen = 0;
        int he = h3fr_extract_preview(cur, &prev, &plen);
        if (he != H3FR_OK || !prev || plen == 0) {
            LOGE("real: 3FR preview 提取 %s 失败: %s", cur, h3fr_strerror(he));
            free(prev);
            be_err("3FR preview 提取失败(%s)", h3fr_strerror(he));
            return -1;
        }
        snprintf(prev_tmp, sizeof prev_tmp, "%s/prev_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        FILE *pf = fopen(prev_tmp, "wb");
        if (!pf || fwrite(prev, 1, plen, pf) != plen) {
            LOGE("real: 3FR preview 落盘 %s 失败", prev_tmp);
            free(prev);
            if (pf) fclose(pf);
            remove(prev_tmp);
            be_err("3FR preview 落盘失败");
            return -1;
        }
        fclose(pf);
        free(prev);
        snprintf(src_path, sizeof src_path, "%s", prev_tmp);
        LOGI("real: 3FR %s → 内嵌 preview %zu B, HQ 管线", cur, plen);
    } else if (suffix_is(cur, ".heic") || suffix_is(cur, ".heif")) {
        /* §26: HEIC → heif_thumb 子进程提取缩略图 JPEG(上游 libheif+de265) */
        snprintf(prev_tmp, sizeof prev_tmp, "%s/heic_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s",
                 X2P_ROOT, cur, prev_tmp);
        int hrc = system(hcmd);
        /* 验产物: JPEG SOI 头 */
        int ok = 0;
        if (hrc == 0) {
            FILE *hf = fopen(prev_tmp, "rb");
            if (hf) { unsigned char soi[2];
                ok = (fread(soi, 1, 2, hf) == 2 && soi[0] == 0xFF && soi[1] == 0xD8);
                fclose(hf); }
        }
        if (!ok) {
            LOGE("real: HEIC %s heif_thumb 失败(rc=%d)", cur, hrc);
            remove(prev_tmp);
            be_err("HEIC 解码失败(不支持的 HEIF 变体或缺缩略图)");
            return -1;
        }
        snprintf(src_path, sizeof src_path, "%s", prev_tmp);
        LOGI("real: HEIC %s → heif_thumb 缩略图 JPEG", cur);
    }

    target_for_file(src_path, paper_w, paper_h, edit, &tw, &th);
    e = imgprep_prepare_edit(src_path, dst, tw, th, is_3fr ? 0 : 92, 0, edit, rep);
    if (prev_tmp[0]) remove(prev_tmp);             /* preview 临时件即弃 */
    if (e != 0) {
        LOGE("real: imgprep %s 失败: %s", cur, imgprep_strerror(e));
        be_err("图像处理失败: %s%s", imgprep_strerror(e),
               e == IMGPREP_E_ARG ? "(裁切/旋转参数非法)" : "");
        return -1;
    }
    return 0;
}

static void rj_finish(const char *status, const char *why)
{
    if (why) LOGW("real: job -> %s (%s)", status, why);
    if (!strcmp(status, "done")) {
        prn_set_job_progress(100);
        g_rj.phase = RJ_DONE;
    } else if (!strcmp(status, "canceled")) {
        g_rj.phase = RJ_CANCELED;
    } else {
        g_rj.phase = RJ_FAILED;
    }
    rj_set_status(status, why);
    prn_set_printer_state(!strcmp(status, "failed") ? "error" : "idle");
    g_rj.t0 = now_ms();
}

static void tick_job(void)
{
    long long now = now_ms();

    switch (g_rj.phase) {
    case RJ_PREP: {
        /* 逐文件 imgprep(§18: 3FR 分流+裁切/旋转+目标横竖自适应收进 rj_prep_file,
         * 与 rb_preview_print 同管线=所见即所得), 每 tick 一个文件 */
        const char *cur = g_rj.files[g_rj.cur];
        imgprep_report_t rep;
        if (rj_prep_file(cur, g_rj.tmps[g_rj.cur], g_rj.paper_w, g_rj.paper_h,
                         &g_rj.edit, &rep) != 0) {
            rj_finish("failed", suffix_is(cur, ".3fr") ? "3fr prep" : "imgprep");
            return;
        }
        LOGI("real: prep %d/%d %s → %dx%d (%ld B, rot=%d°, crop=%d,%d %dx%d)",
             g_rj.cur + 1, g_rj.nfiles, cur, rep.out_w, rep.out_h, rep.out_bytes,
             rep.rotation, rep.ucrop_x, rep.ucrop_y, rep.ucrop_w, rep.ucrop_h);
        if (++g_rj.cur >= g_rj.nfiles) {
            g_rj.cur = 0;
            g_rj.phase = RJ_SEND;
            g_rj.last_poll = 0;
            prn_set_job_progress(10);
            rj_set_status("sending", NULL);
        }
        break;
    }
    case RJ_SEND: {
        /* 读中间产物 → Print-Job JPEG 直发(显式 document-format=image/jpeg) */
        FILE *f = fopen(g_rj.tmps[g_rj.cur], "rb");
        if (!f) { rj_finish("failed", "open prep output"); return; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *jpeg = malloc(len > 0 ? (size_t)len : 1);
        if (!jpeg || (len > 0 && fread(jpeg, 1, (size_t)len, f) != (size_t)len)) {
            free(jpeg);
            fclose(f);
            rj_finish("failed", "read prep output");
            return;
        }
        fclose(f);

        ipp_print_opts_t o;
        memset(&o, 0, sizeof o);
        o.copies = g_rj.copies;
        o.color_mode = g_rj.color;                  /* 原生关键字(能力校验过) */
        o.paper = g_rj.media;
        o.quality = g_rj.quality_enum;
        o.print_scaling = g_rj.scaling[0] ? g_rj.scaling : NULL;
        o.job_name = "x2print";
        o.document_format = "image/jpeg";           /* 格式协商结论(默认 urf 的打印机必须显式) */

        int job_id = 0;
        int e = ipp_print_jpeg(g_ip, g_port, jpeg, (size_t)len, &o, &job_id);
        free(jpeg);
        if (e != IPP_OK) {
            LOGE("real: Print-Job %d/%d 失败: %s (%s)", g_rj.cur + 1, g_rj.nfiles,
                 ipp_strerror(e), ipp_last_detail());
            rj_finish("failed", "ipp print");
            return;
        }
        g_rj.ipp_ids[g_rj.cur] = job_id;
        LOGI("real: sent %d/%d → ipp job %d (copies=%d media=%s)", g_rj.cur + 1,
             g_rj.nfiles, job_id, o.copies, o.paper);
        g_rj.cur++;
        prn_set_job_progress(10 + (30 * g_rj.cur) / g_rj.nfiles);
        if (g_rj.cur >= g_rj.nfiles) {
            g_rj.cur = 0;
            g_rj.phase = RJ_PRINT;
            g_rj.last_poll = 0;
            rj_set_status("printing", NULL);
            prn_set_job_progress(40);
        }
        break;
    }
    case RJ_PRINT: {
        /* 总预算按页数伸缩: 基础 2min + 75s/页(既单张 3min15s; 多张线性) */
        long long budget = RJ_TOTAL_TIMEOUT_MS_BASE
                         + (long long)RJ_TOTAL_TIMEOUT_MS_PAGE * g_rj.nfiles * (g_rj.copies > 0 ? g_rj.copies : 1);
        if (now - g_rj.t0 > budget) { rj_finish("failed", "total timeout"); return; }
        if (g_rj.last_poll && now - g_rj.last_poll < RJ_POLL_MS) return;
        g_rj.last_poll = now;

        int id = g_rj.ipp_ids[g_rj.cur];
        /* §23: 轮询子进程化——主循环不再被 10s 级 recv 阻塞(Cancel 曾排队 8-14s);
         * 子进程输出 "ST\t<state>", 超时由 child_run 预算兜底 */
        static poll_arg_t pa;
        if (!g_rj.poll_armed) {
            snprintf(pa.ip, sizeof pa.ip, "%.63s", g_ip);
            pa.port = g_port; pa.id = id;
            if (child_run("job poll", 12000, poll_child, &pa) == 0)
                g_rj.poll_armed = 1;
            return;                                    /* 下轮收尸 */
        }
        char po[64];
        child_poll(po, sizeof po);
        g_rj.poll_armed = 0;
        int jstate = -1;
        if (sscanf(po, "ST\t%d", &jstate) != 1) {
            LOGW("real: poll ipp job %d 失败/超时 (下轮重试)", id);
            return;                                    /* 单次轮询失败容忍 */
        }
        if (jstate == 9) {                             /* completed */
            g_rj.cur++;
            g_rj.done_jobs++;
            if (g_rj.cur >= g_rj.nfiles) { rj_finish("done", NULL); return; }
            prn_set_job_progress(40 + (55 * g_rj.done_jobs) / g_rj.nfiles);
        } else if (jstate == 7) {
            rj_finish("canceled", "ipp job canceled");
        } else if (jstate == 8) {
            rj_finish("failed", "ipp job aborted");
        } else {                                       /* pending/processing: 缓爬 */
            int p = 40 + (55 * g_rj.done_jobs) / g_rj.nfiles
                    + 2 * (int)((now - g_rj.t0) / 4000);
            if (p > 95) p = 95;
            prn_set_job_progress(p);
        }
        break;
    }
    case RJ_DONE:
    case RJ_FAILED:
    case RJ_CANCELED:
        if (now - g_rj.t0 >= RJ_HOLD_MS) {             /* 终值保持后回 idle */
            g_rj.phase = RJ_IDLE;
            rj_free_files();
            rj_set_status("idle", NULL);
            prn_set_job_progress(0);
            prn_set_printer_state("idle");
        }
        break;
    case RJ_IDLE:
    default:
        break;
    }
}

/* ---------------- Wi-Fi 连接编排(tick 步进, P0: 处理器受理即返) ----------------
 * 相机真路径(wifi_cam 阶段机, NOTES §15): 让出原生 AP → 会话防护 →
 * insmod(如需)→ifup→wpa_supplicant→COMPLETED→udhcpc, 全部子进程+tick 推进。
 * psk 经 Connect_to_ap 传入后由 wifi_cam 落 /data/x2print/psk(0600),
 * 本层不留驻不留日志。
 * 合法电源协议(§4b): 会话开始探测原生 AP 是否在跑(用户在用)——在跑则先
 * power_on_wifi(false) 合法让出并记忆(+ap_was_on.saved 落盘凭据, P1②崩溃
 * 恢复); 恢复末尾按记忆回原状(原开→power_on_wifi(true), 原关→合法关),
 * 绝不让 camera-system 状态机与实际失同步。
 * P1①: join 失败回滚与成功恢复走同一条恢复机(WJ_RESTORE_*), 电源同步+
 * guard_off 一个不少; 拆分 WJ_GUARD_A/B 与 WJ_YIELD_* 使每 tick 至多一次
 * 阻塞 D-Bus 往返(≤2s) —— 主循环呼吸预算成立。 */
enum {
    WJ_IDLE = 0,
    WJ_MOCK_WAIT,         /* 联调: 1.2s 模拟 */
    WJ_YIELD_AP,          /* 探测原生 AP + 合法让出(power(0)) */
    WJ_YIELD_WAIT,        /* 让出后 1s 余量(tick 时钟) */
    WJ_GUARD_A,           /* 会话防护前半: wakelock + 读屏超时原值落盘凭据 */
    WJ_GUARD_B,           /* 会话防护后半: 覆写 screen_idle_timeout */
    WJ_JOIN,              /* wifi_cam 阶段机 */
    WJ_RESTORE_WIFI,      /* 恢复机: wifi 反序(rollback 与 disconnect 共用) */
    WJ_RESTORE_POWER,     /* 合法电源同步(回会话前原状) */
    WJ_RESTORE_GUARD,     /* 会话防护释放 */
    WJ_RESTORE_FINAL,     /* 清场 + disconnected */
};
static struct {
    int  phase;
    int  full_clear;      /* 恢复收尾是否清现场(回滚也清: 失败尝试的 ssid/psk 同为会话态) */
    long long t0;
    char ssid[PRN_SLEN];
    char psk[PRN_SLEN];
} g_wj;
static int g_ap_was_on = -1;    /* 会话开始时原生 AP 状态: 1=在跑 0=未跑 -1=未探测
                                 * (§20: 扫描让出过芯片则已记忆, Connect 不再覆盖) */
static int g_scan_kept = 0;     /* §20: Scan_wifi 后芯片保持可用(未连接未恢复) ——
                                 * 闲置看门狗据此兜底恢复( disconnected 态也计时) */
static int if_addr_netmask(const char *ifname, struct in_addr *addr, struct in_addr *mask); /* 前置 */
static void session_guard_a(void);     /* §19 会话-待机防护(定义见文件尾; P0 拆两 tick) */
static void session_guard_b(void);
static void session_guard_off(void);
static void legal_power_sync(void);

/* AP 原状凭据(P1②, screen_timeout.saved 同款): 让出原生 AP 时落盘,
 * 正常恢复(legal_power_sync)消费删除; 服务暴毙后 boot_init 据此重拉 AP */
#define AP_WAS_ON_FILE "/data/x2print/ap_was_on.saved"

static void ap_was_on_save(void)
{
    FILE *f = fopen(AP_WAS_ON_FILE, "w");
    if (f) { fputs("1\n", f); fclose(f); }
}

/* 原生 AP 在跑探测(免 shell): hostapd 进程在, 或 wlan0 已配原生 AP 地址 192.168.2.1 */
static int native_ap_running(void)
{
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        char fn[64], comm[64];
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            snprintf(fn, sizeof fn, "/proc/%.20s/comm", de->d_name);
            FILE *f = fopen(fn, "r");
            if (!f) continue;
            if (fgets(comm, sizeof comm, f) && !strncmp(comm, "hostapd", 7)) {
                fclose(f);
                closedir(d);
                return 1;
            }
            fclose(f);
        }
        closedir(d);
    }
    struct in_addr a, m;
    if (if_addr_netmask("wlan0", &a, &m) == 0) {
        char ip[32];
        snprintf(ip, sizeof ip, "%s", inet_ntoa(a));
        if (!strcmp(ip, "192.168.2.1")) return 1;      /* 原生 AP 网关地址 */
    }
    return 0;
}

/* 合法电源协议②(P1①: 回滚与断开恢复同走): 恢复末尾同步 camera-system 状态机
 * 回会话前原状。只在首次恢复执行(重复 disconnect 幂等跳过, 防反复 restart AP);
 * 凭据随之消费删除。 */
static void legal_power_sync(void)
{
    /* §23 定案: 恢复一律合法关电——AP 配置从未被本服务改动(hostapd.conf 原样),
     * 无需"还原配置"; 重开 AP 交给用户(Phocus 蓝牙随时可唤醒, 且原厂有闲置自拆纪律) */
    g_ap_was_on = -1;
    remove(AP_WAS_ON_FILE);                        /* 陈旧凭据一并清 */
    if (prn_adaptor_system_wifi_power(0) != 0)
        LOGW("real: 合法关电同步失败——camera-system 状态可能失同步, 可 service.sh verify 核查");
    else
        LOGI("real: 合法关电完成(AP 配置未动, 需要时 Phocus/verify 可随时拉起)");
}

/* 恢复收尾: 清打印现场(仅 disconnect 语义) + disconnected 广播 */
static void restore_finalize(int full_clear)
{
    if (full_clear) {
        g_ip[0] = 0;
        eps_clear();
        prn_caps_invalidate();                             /* 能力随链路失效 */
        prn_discovered_clear();
        prn_set_printer_name("");
        /* printer_ip=纯内部会话缓存(发现学得回写): 恢复时无条件清除,
         * 下次会话从 L2 重新发现(勿手配, 会被清) */
        prn_config_set("printer_ip", "");
        prn_set_printer_ip("");
        /* ssid/psk 同为会话态: 恢复即清(下次会话从扫描重新来; 偏好项 copies/
         * quality/color 不动)。ssid 经 prn_set_string 落 config, 置空即同步。 */
        prn_set_string(PRN_SSID, "");
        wifi_cam_psk_purge();
    }
    g_scan_kept = 0;                                       /* §20: 扫描保留态一并收尾 */
    prn_set_connection_state("disconnected");
    LOGI("real: disconnected, wifi restored (idle=%.0fs)",
         prn_adaptor_idle_ms() / 1000.0);
}

static void wj_join_done(int ok, const char *how)
{
    long long dt = now_ms() - g_wj.t0;
    memset(g_wj.psk, 0, sizeof g_wj.psk);              /* psk 不留驻内存 */
    if (ok) {
        g_wj.phase = WJ_IDLE;
        prn_set_connection_state("joined");
        LOGI("real: joined via %s (%.1fs 总耗时)", how, dt / 1000.0);
        return;
    }
    LOGW("real: join failed: %s (%.1fs), 先恢复基线再回 disconnected", how, dt / 1000.0);
    if (!wifi_is_mock()) {
        /* P1①: 失败回滚与成功恢复同走恢复机(电源还原+guard_off 一个不少) */
        wifi_cam_restore_begin();
        g_wj.full_clear = 1;   /* 回滚也清: 失败尝试不留 ssid/psk 残留 */
        g_wj.phase = WJ_RESTORE_WIFI;
        return;
    }
    g_wj.phase = WJ_IDLE;
    prn_set_connection_state("disconnected");
}

/* 连接前置(Connect/Scan 共用): 探测原生 AP, 在跑则合法让出+记忆+落凭据。
 * 返回下一 phase(调用方上下文); 每次调用至多一次阻塞 power_on_wifi(≤2s)。 */
static void tick_join(void)
{
    switch (g_wj.phase) {
    case WJ_MOCK_WAIT:                                 /* 联调: 1.2s 模拟关联+DHCP */
        if (now_ms() - g_wj.t0 >= 1200) wj_join_done(1, "wifi_backend=mock");
        break;
    case WJ_YIELD_AP: {                                /* 合法电源协议① */
        if (g_ap_was_on < 0)
            g_ap_was_on = native_ap_running();
        if (g_ap_was_on == 1) {
            LOGI("real: 原生 AP 在跑(hostapd/192.168.2.1) → power_on_wifi(false) 合法让出");
            ap_was_on_save();                          /* P1②: 崩溃恢复凭据先落盘 */
            prn_adaptor_system_wifi_power(0);
            g_wj.phase = WJ_YIELD_WAIT;                /* 原生路径停 AP 的余量 */
            g_wj.t0 = now_ms();
        } else {
            g_wj.phase = WJ_GUARD_A;
        }
        break;
    }
    case WJ_YIELD_WAIT:
        if (now_ms() - g_wj.t0 >= 1000) g_wj.phase = WJ_GUARD_A;
        break;
    case WJ_GUARD_A:                                   /* §19 防护前半(wakelock+读原值) */
        session_guard_a();
        g_wj.phase = WJ_GUARD_B;
        break;
    case WJ_GUARD_B:                                   /* §19 防护后半(覆写息屏) */
        session_guard_b();
        if (wifi_cam_join_start(g_wj.ssid, g_wj.psk) != 0) {  /* psk 交 wifi_cam 落盘 */
            be_err("相机 Wi-Fi 编排启动失败(缺 wpa 二进制/参数非法, 见服务日志)");
            wj_join_done(0, "wifi_cam_join_start");
            break;
        }
        g_wj.phase = WJ_JOIN;
        break;
    case WJ_JOIN: {                                    /* 相机真路径步进 */
        int r = wifi_cam_join_step();                  /* 0=joined 1=进行中 -1=失败 */
        if (r == 0) wj_join_done(1, wifi_cam_join_phase());
        else if (r < 0) wj_join_done(0, wifi_cam_join_phase());
        break;
    }
    case WJ_RESTORE_WIFI:                              /* wifi 反序恢复(幂等) */
        if (wifi_cam_restore_step() == 0) g_wj.phase = WJ_RESTORE_POWER;
        break;
    case WJ_RESTORE_POWER:                             /* P1①: 合法电源同步回原状 */
        legal_power_sync();
        g_wj.phase = WJ_RESTORE_GUARD;
        break;
    case WJ_RESTORE_GUARD:                             /* §19: 在电源同步后释放防护 */
        session_guard_off();
        g_wj.phase = WJ_RESTORE_FINAL;
        break;
    case WJ_RESTORE_FINAL:
        restore_finalize(g_wj.full_clear);
        g_wj.phase = WJ_IDLE;
        break;
    default:
        break;
    }
}

/* ---------------- 后端函数表实现 ---------------- */
static int rb_connect_to_ap(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return PRN_BE_PARAM;
    if (g_wj.phase != WJ_IDLE) return PRN_BE_BUSY;
    if (!prn_state_is("disconnected")) return PRN_BE_BUSY;
    /* §20: psk 空串=开放网络(免密直连, wifi_cam 落 key_mgmt=NONE); 1..7 位拒绝 */
    if (!psk || (psk[0] && strlen(psk) < 8)) {
        LOGW("real: psk invalid len %zu (empty=open, 8..63=wpa2)", psk ? strlen(psk) : 0);
        return PRN_BE_FAILED;
    }
    prn_set_string(PRN_SSID, ssid);                    /* 落 config.ini(psk 不落) */
    prn_set_connection_state("joining");
    snprintf(g_wj.ssid, sizeof g_wj.ssid, "%s", ssid);
    snprintf(g_wj.psk, sizeof g_wj.psk, "%s", psk);
    g_wj.t0 = now_ms();
    g_wj.full_clear = 0;   /* 受理初值: 成功路径后由 disconnect 置 1; 失败回滚路径会改 1 */
    if (wifi_is_mock()) {
        g_wj.phase = WJ_MOCK_WAIT;
    } else {
        /* 让出/防护/join 全在 tick 机(P0: 受理即返, 与 Connect 语义一致) */
        g_wj.phase = WJ_YIELD_AP;
    }
    LOGI("real: joining '%s' via wifi_backend=%s", ssid,
         wifi_is_mock() ? "mock" : "cam(让出→防护→insmod→supplicant→dhcp)");
    return 0;                                          /* 受理; tick 推进 */
}

/* ---------------- §20 SSID 扫描选择(INTERFACE.md v2.2 §3) ----------------
 * doScan_wifi → as(元素 "SSID\tRSSI(dBm)\tSEC"), 结果同步进 scanned_networks(notify)。
 * 时序: Scan_wifi → GUI 列表选择 → Connect_to_ap(ssid, psk) 不变。
 * 芯片语义: 前置与 Connect_to_ap 相同(wlan0 缺失则 insmod+up, 原生 AP 在跑先合法
 * 让出并记忆); 扫描后**保持芯片可用不拆除**(用户即将连接) —— 若放弃由闲置看门狗
 * (§20 已扩展到 disconnected+扫描态)或 Disconnect_and_restore 收尾。会话防护
 * (§19 屏常亮+wakelock)自扫描受理起持有, 保证看门狗恢复时 power_on_wifi(原状)
 * 在屏亮下被执行。
 * P0: 真路径改为受理+tick 机, 方法回复延迟至扫描完成(adaptor 挂起回复机制,
 * 线协议契约不变「阻塞至完成」); mock 路径保持同步(2s 有界)。 */
/* as 数组释放小工具(adaptor.c 同款为 static, 此处各持一份) */
static void free_str_array_local(char **items, int n)
{
    if (!items) return;
    for (int i = 0; i < n; i++) free(items[i]);
    free(items);
}

enum { WS_IDLE = 0, WS_YIELD, WS_YIELD_WAIT, WS_GUARD_A, WS_GUARD_B, WS_SCAN };
static struct { int phase; long long t0; } g_ws;

/* 扫描机中断(Disconnect): 挂起回复补 failed, 机器复位 */
static void ws_abort(void)
{
    if (g_ws.phase == WS_IDLE) return;
    LOGW("real: scan_wifi 中断(断开/恢复), 挂起回复补 failed");
    be_err("扫描被断开/恢复中断");
    g_ws.phase = WS_IDLE;
    prn_adaptor_scan_complete(0);
}

static void ws_finish_ok(char **items, int cnt)
{
    prn_scanned_clear();
    for (int i = 0; i < cnt && i < PRN_MAX_SCAN; i++)
        prn_scanned_add(items[i]);
    free_str_array_local(items, cnt);
    g_scan_kept = 1;                    /* 芯片保持可用(看门狗/restore 收尾) */
    g_ws.phase = WS_IDLE;
    prn_adaptor_scan_complete(1);       /* 挂起回复: 与 scanned_networks 同源 */
    LOGI("real: scan_wifi done, %d network(s) published (scanned_networks notify)",
         prn_state_get()->scanned.n);
}

static void tick_scan(void)
{
    switch (g_ws.phase) {
    case WS_YIELD:                                      /* 合法电源协议①(扫描版) */
        if (g_ap_was_on < 0)
            g_ap_was_on = native_ap_running();
        if (g_ap_was_on == 1) {
            LOGI("real: 原生 AP 在跑 → power_on_wifi(false) 合法让出(扫描用, 原状已记忆)");
            ap_was_on_save();
            prn_adaptor_system_wifi_power(0);
            g_ws.phase = WS_YIELD_WAIT;
            g_ws.t0 = now_ms();
        } else {
            g_ws.phase = WS_GUARD_A;
        }
        break;
    case WS_YIELD_WAIT:
        if (now_ms() - g_ws.t0 >= 1000) g_ws.phase = WS_GUARD_A;
        break;
    case WS_GUARD_A:
        session_guard_a();                              /* §19 扫描即进入会话防护 */
        g_ws.phase = WS_GUARD_B;
        break;
    case WS_GUARD_B:
        session_guard_b();
        if (wifi_cam_scan_begin() != 0) {
            be_err("相机 Wi-Fi 扫描启动失败(芯片/二进制问题, 见服务日志)");
            g_scan_kept = 1;                            /* 失败也交看门狗收 */
            g_ws.phase = WS_IDLE;
            prn_adaptor_scan_complete(0);
            break;
        }
        g_ws.phase = WS_SCAN;
        break;
    case WS_SCAN: {
        char **items = NULL;
        int cnt = 0;
        int r = wifi_cam_scan_step(&items, &cnt);
        if (r == 0) { ws_finish_ok(items, cnt); break; }
        if (r < 0) {
            be_err("相机 Wi-Fi 扫描失败(芯片/二进制问题, 见服务日志)");
            g_scan_kept = 1;
            g_ws.phase = WS_IDLE;
            prn_adaptor_scan_complete(0);
        }
        break;
    }
    default:
        break;
    }
}

static int rb_scan_wifi(char ***out, int *n)
{
    *out = NULL;
    *n = 0;
    if (g_wj.phase != WJ_IDLE)
        return PRN_BE_BUSY;                            /* join 进行中 */
    if (!prn_state_is("disconnected"))
        return PRN_BE_BUSY;                            /* joining/joined/ready/discovering */
    if (g_scan_kept == 2 || g_ws.phase != WS_IDLE)
        return PRN_BE_BUSY;                            /* 扫描进行中(单线程下防御) */

    if (wifi_is_mock()) {                              /* 联调: 2s 假扫描, 4 假网络 */
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        static const char *const nets[] = {
            "hannto-printer-honey1s_mibt4D0F\t-50\twpa2",
            "guguji-IOT\t-58\twpa2",
            "Coffee-Guest\t-72\topen",
            "HomeNet_5G\t-80\twpa2",
        };
        char **items = calloc(sizeof nets / sizeof nets[0] + 1, sizeof(char *));
        if (!items) return PRN_BE_FAILED;
        int cnt = 0;
        for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
            items[cnt++] = strdup(nets[i]);
        prn_scanned_clear();
        for (int i = 0; i < cnt && i < PRN_MAX_SCAN; i++)
            prn_scanned_add(items[i]);
        free_str_array_local(items, cnt);
        prn_state *st = prn_state_get();
        char **reply = calloc((size_t)st->scanned.n + 1, sizeof(char *));
        if (!reply) return PRN_BE_FAILED;
        for (int i = 0; i < st->scanned.n; i++)
            reply[i] = strdup(st->scanned.items[i]);
        *out = reply;
        *n = st->scanned.n;
        LOGI("real: scan_wifi done, %d network(s) published (scanned_networks notify)",
             st->scanned.n);
        return 0;
    }

    /* 真路径: 受理即返(回复由 adaptor 挂起至完成, 契约不变); 机器 tick 推进 */
    g_ws.phase = WS_YIELD;
    g_ws.t0 = now_ms();
    g_scan_kept = 2;                    /* 扫描进行中 */
    LOGI("real: scan_wifi 受理(让出→防护→芯片→扫描, tick 推进; 挂起回复至完成)");
    return 1;                           /* 1=异步受理(adaptor 延迟回复) */
}

/* 断开+恢复: 幂等且完备(INTERFACE.md §4b) —— 任何 connection_state、重复调用、
 * join/扫描/发现进行中调用, 都收敛到 disconnected + 相机 Wi-Fi 基线(零残留)。
 * P0: 受理即返 —— 在途子进程(insmod/udhcpc/探测)立即 SIGKILL, 恢复序列
 * (wifi 反序→合法电源同步→防护释放→清场)由 tick 推进, 完成时
 * propertiesChanged(connection_state→disconnected) 广播可见。 */
static void disc_abort(void);                   /* 发现机中断(定义在发现区) */
static int rb_disconnect_restore(void)
{
    if (g_wj.phase >= WJ_RESTORE_WIFI) {
        LOGI("real: disconnect: 恢复已在进行中(幂等受理)");
        return 0;
    }
    if (prn_job_active()) {                            /* 拆链=强取消 */
        g_rj.phase = RJ_CANCELED;
        g_rj.t0 = now_ms();
        rj_set_status("canceled", "disconnect");
        prn_set_printer_state("idle");
        cancel_job_async();                             /* IPP 取消子进程(尽力而为) */
    }
    /* P0 中断位: join/扫描/发现的一切在途步骤立即终结 */
    wifi_cam_abort();
    child_abort();                                      /* 发现探测/取消环子进程 */
    disc_abort();
    ws_abort();
    memset(g_wj.psk, 0, sizeof g_wj.psk);

    if (!wifi_is_mock()) {
        wifi_cam_restore_begin();                       /* 相机: 反序恢复(幂等尽力而为) */
        g_wj.full_clear = 1;
        g_wj.phase = WJ_RESTORE_WIFI;                   /* tick 推进至 disconnected */
        LOGI("real: disconnect 受理, 恢复序列启动(wifi→电源同步→防护释放→清场)");
        return 0;
    }
    restore_finalize(1);                                /* mock: 立即完成 */
    return 0;
}

/* TXT 串("k=v;k=v")提取 rp= 值(AirPrint 端点路径, 如 "ipp/print"); 找不到返回 NULL */
static const char *txt_rp(const char *txt)
{
    static char rp[96];
    if (!txt) return NULL;
    const char *p = txt;
    while (p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 3 && !strncmp(p, "rp=", 3)) {
            size_t c = len - 3 < sizeof rp - 1 ? len - 3 : sizeof rp - 1;
            memcpy(rp, p + 3, c);
            rp[c] = 0;
            return rp;
        }
        p = semi ? semi + 1 : NULL;
    }
    return NULL;
}

/* "host[:port]" 解析; 返回 port(无=0), host 拷入 out */
static int parse_hostport(const char *s, char *host, size_t cap)
{
    snprintf(host, cap, "%s", s);
    char *colon = strrchr(host, ':');
    if (colon) {
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) {
            *colon = 0;
            return port;
        }
        *colon = 0;                                    /* 非法端口: 当作 host 一部分丢弃 */
    }
    return 0;
}

/* ---------------- 发现分步状态机(P0: 受理即返 + tick 推进, NOTES §22) ----------------
 * doStart_discovery 受理即返(返回空 as; 与 Connect_to_ap 同语义), L1→L2→L3→L4
 * 各级为 tick 状态, 每端点 IPP 验明/mDNS 窗口//24 扫描都在子进程里(黑洞端点
 * 由预算 SIGKILL 快速放弃), 结果经 discovered_printers 逐项 notify, 完成即
 * connection_state 离开 discovering。总预算 60s, 到点即返已发现结果。 */
#define DISC_BUDGET_MS  60000   /* 发现总预算 */
#define DISC_PROBE_MS    15000  /* 单端点验明预算(整体, 父进程 kill 兜底); 冷启动容忍 */
#define DISC_MDNS_MS     8000   /* mDNS 窗口(子进程, 自身 6s) */
#define DISC_SCAN_MS     6000   /* /24 快扫(48/批×350ms ≈2s, 预算 6s) */

enum {
    D_IDLE = 0,
    D_L1,          /* printer_ip 缓存端点逐个验明 */
    D_MDNS,        /* mDNS 窗口(子进程) */
    D_MDNS_PROBE,  /* mDNS 候选逐个验明 */
    D_GW,          /* 默认网关验明 */
    D_GW_PROBE,
    D_SCAN,        /* /24 快扫(子进程) */
    D_SCAN_PROBE,  /* 命中逐个验明(≤4) */
    D_FINISH,
};
static struct {
    int  phase;
    long long t0;
    int  defport, port;
    /* L1 端点表 */
    char ep_ip[8][80]; int ep_port[8]; int n_eps, eps_i;
    /* L2 mDNS 候选 */
    char m_ip[8][64]; int m_port[8]; char m_rp[8][96]; char m_inst[8][160];
    int  m_univ[8]; int n_mdns, mdns_i;
    /* L4 命中 */
    char hit[8][32]; int n_hits, hit_i;
    char gw[32]; int have_gw;
    int  found;
    int  m_started;                 /* D_MDNS 窗口子进程已起 */
    probe_arg_t parg;       /* 当前探测参数(子进程读) */
    scan_arg_t sarg;
    char out[8192];
} g_disc;

static void disc_abort(void)
{
    if (g_disc.phase == D_IDLE) return;
    LOGW("real: discovery 中断(断开/恢复), 已发现 %d 台弃用", g_disc.found);
    g_disc.phase = D_IDLE;
}

/* 探测结果落库: 子进程回 "OK\t<名字>" → 登记侧表+发现列表 */
static void disc_probe_collect(const char *ip, int port, const char *path)
{
    if (strncmp(g_disc.out, "OK\t", 3) == 0) {
        char name[PRN_SLEN];
        snprintf(name, sizeof name, "%.95s", g_disc.out + 3);
        /* 去行尾换行 */
        char *nl = strchr(name, '\n');
        if (nl) *nl = 0;
        if (!name[0]) snprintf(name, sizeof name, "%s", "IPP Printer");
        if (eps_add(name, ip, port, path) >= 0) {
            char el[PRN_SLEN + 80];
            snprintf(el, sizeof el, "%s\t%s", name, ip);
            prn_discovered_add(el);
            g_disc.found++;
            LOGI("real: probe %s:%d → '%s' 入列", ip, port ? port : IPP_DEF_PORT, name);
            return;
        }
    } else {
        LOGW("real: probe %s:%d failed(黑洞/非 IPP, %.1fs 预算内放弃)", ip,
             port ? port : IPP_DEF_PORT, (now_ms() - g_ch.t0) / 1000.0);
    }
}

/* printer_ip 回写(纯内部会话缓存: 发现学得, 恢复时无条件清除) */
static void disc_writeback(void)
{
    if (g_disc.found <= 0 || g_eps_n <= 0) return;
    char cache[300];
    size_t off = 0;
    cache[0] = 0;
    for (int i = 0; i < g_eps_n; i++) {
        char one[96];
        if (g_eps[i].port > 0)
            snprintf(one, sizeof one, "%s%s:%d", off ? "," : "",
                     g_eps[i].ip, g_eps[i].port);
        else
            snprintf(one, sizeof one, "%s%s", off ? "," : "", g_eps[i].ip);
        size_t ol = strlen(one);
        if (off + ol + 1 >= sizeof cache) break;
        memcpy(cache + off, one, ol + 1);
        off += ol;
    }
    const char *cur = prn_config_get("printer_ip");
    if (strcmp(cache, cur) != 0) {
        prn_config_set("printer_ip", cache);
        LOGI("real: 发现结果回写 printer_ip = %s(内部缓存, 恢复时清除)", cache);
    }
}

static void disc_finish(void)
{
    disc_writeback();
    ipp_set_path(NULL);                                /* 探测用路径复位(选中时再设) */
    prn_set_connection_state("joined");                /* 扫描完成(直连模式=网络可用) */
    LOGI("real: discovery done, %d found (%.1fs)", g_disc.found,
         (now_ms() - g_disc.t0) / 1000.0);
    g_disc.phase = D_IDLE;
}

static void disc_spawn_probe(const char *ip, int port, const char *path)
{
    snprintf(g_disc.parg.ip, sizeof g_disc.parg.ip, "%.63s", ip);
    g_disc.parg.port = port;
    snprintf(g_disc.parg.path, sizeof g_disc.parg.path, "%.95s", path ? path : "");
    child_run("ipp probe", DISC_PROBE_MS, probe_child, &g_disc.parg);
}

static int default_gw(char *gw, size_t cap);    /* 定义在后(发现辅助区) */

static void tick_discovery(void)
{
    if (g_disc.phase == D_IDLE) return;
    if (now_ms() - g_disc.t0 > DISC_BUDGET_MS && g_disc.phase != D_FINISH) {
        LOGW("real: discovery 总预算 %ds 到点, 返已发现结果(%d 台)", DISC_BUDGET_MS / 1000,
             g_disc.found);
        g_disc.phase = D_FINISH;
    }

    switch (g_disc.phase) {
    case D_L1:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;   /* 在跑 */
        if (g_disc.eps_i > 0)                          /* 上一端点收尸落库 */
            disc_probe_collect(g_disc.ep_ip[g_disc.eps_i - 1],
                               g_disc.ep_port[g_disc.eps_i - 1], NULL);
        if (g_disc.eps_i < g_disc.n_eps) {
            int i = g_disc.eps_i++;
            LOGI("real: L1 直连探测 %s:%d (printer_ip 缓存 %d/%d)",
                 g_disc.ep_ip[i], g_disc.ep_port[i] ? g_disc.ep_port[i] : IPP_DEF_PORT,
                 i + 1, g_disc.n_eps);
            disc_spawn_probe(g_disc.ep_ip[i], g_disc.ep_port[i], NULL);
            g_disc.phase = D_L1;
            return;
        }
        g_disc.phase = (g_disc.found == 0) ? D_MDNS : D_FINISH;
        return;
    case D_MDNS:
        if (!g_disc.m_started) {                        /* 首入: 起 mDNS 窗口子进程 */
            if (child_run("mdns window", DISC_MDNS_MS, mdns_child, NULL) != 0) {
                LOGW("real: L2 mDNS 子进程 spawn 失败, 跳过");
                g_disc.phase = D_GW;
                return;
            }
            g_disc.m_started = 1;
            return;
        }
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        g_disc.n_mdns = 0;
        for (char *line = strtok(g_disc.out, "\n"); line; line = strtok(NULL, "\n")) {
            char ip[64], inst[160], txt[768];
            int port = 0, univ = 0;
            if (sscanf(line, "SVC\t%63s\t%d\t%d\t%159[^\t]\t%767[^\n]",
                       ip, &port, &univ, inst, txt) == 5 && g_disc.n_mdns < 8) {
                int i = g_disc.n_mdns++;
                snprintf(g_disc.m_ip[i], sizeof g_disc.m_ip[i], "%s", ip);
                g_disc.m_port[i] = port;
                g_disc.m_univ[i] = univ;
                const char *rp = txt_rp(txt);
                snprintf(g_disc.m_rp[i], sizeof g_disc.m_rp[i], "%s", rp ? rp : "");
                snprintf(g_disc.m_inst[i], sizeof g_disc.m_inst[i], "%s", inst);
            }
        }
        if (g_disc.n_mdns > 0) {
            g_disc.mdns_i = 0;
            g_disc.phase = D_MDNS_PROBE;
        } else {
            LOGI("real: L2 mDNS 无候选");
            g_disc.phase = D_GW;
        }
        return;
    case D_MDNS_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        if (g_disc.mdns_i > 0) {
            int i = g_disc.mdns_i - 1;
            disc_probe_collect(g_disc.m_ip[i], g_disc.m_port[i], g_disc.m_rp[i]);
        }
        if (g_disc.mdns_i < g_disc.n_mdns) {
            int i = g_disc.mdns_i++;
            char *dot = strstr(g_disc.m_inst[i], "._ipp");   /* 去服务类型后缀 */
            if (dot) *dot = 0;
            LOGI("real: L2 mDNS 候选 '%s' @ %s:%d rp=%s%s → IPP 验明中",
                 g_disc.m_inst[i], g_disc.m_ip[i],
                 g_disc.m_port[i] ? g_disc.m_port[i] : IPP_DEF_PORT,
                 g_disc.m_rp[i][0] ? g_disc.m_rp[i] : "(default)",
                 g_disc.m_univ[i] ? " [AirPrint]" : "");
            disc_spawn_probe(g_disc.m_ip[i], g_disc.m_port[i], g_disc.m_rp[i]);
            return;
        }
        g_disc.phase = (g_disc.found == 0) ? D_GW : D_FINISH;
        return;
    case D_GW: {
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;   /* 不在跑: 落空 */
        g_disc.have_gw = (default_gw(g_disc.gw, sizeof g_disc.gw) == 0);
        if (g_disc.have_gw) {
            LOGI("real: L3 网关探测 %s:%d", g_disc.gw, g_disc.port);
            disc_spawn_probe(g_disc.gw, g_disc.defport, NULL);
            g_disc.phase = D_GW_PROBE;
        } else {
            LOGW("real: L3 无默认路由(/proc/net/route), 跳过");
            g_disc.phase = D_SCAN;
        }
        return;
    }
    case D_GW_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        disc_probe_collect(g_disc.gw, g_disc.defport, NULL);
        g_disc.phase = (g_disc.found == 0) ? D_SCAN : D_FINISH;
        return;
    case D_SCAN: {
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        struct in_addr a, m;
        if (if_addr_netmask("wlan0", &a, &m) == 0) {
            g_disc.sarg.base.s_addr = a.s_addr & m.s_addr;
            g_disc.sarg.port = g_disc.port;
            LOGI("real: L4 /24 扫描 %s/24 :%d", inet_ntoa(g_disc.sarg.base), g_disc.port);
            if (child_run("subnet scan", DISC_SCAN_MS, netscan_child, &g_disc.sarg) == 0) {
                g_disc.phase = D_SCAN_PROBE;
                g_disc.hit_i = 0;
                g_disc.n_hits = 0;
                return;
            }
        } else {
            LOGW("real: L4 取 wlan0 地址失败, 跳过扫描");
        }
        g_disc.phase = D_FINISH;
        return;
    }
    case D_SCAN_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        if (g_disc.hit_i == 0 && g_disc.n_hits == 0) {
            /* /24 快扫结果解析(首入; HIT 行 → 命中表) */
            for (char *line = strtok(g_disc.out, "\n"); line; line = strtok(NULL, "\n")) {
                char ip[32];
                if (sscanf(line, "HIT\t%31s", ip) == 1 && g_disc.n_hits < 8)
                    snprintf(g_disc.hit[g_disc.n_hits++], sizeof g_disc.hit[0], "%s", ip);
            }
        } else if (g_disc.hit_i > 0) {
            disc_probe_collect(g_disc.hit[g_disc.hit_i - 1], g_disc.defport, NULL);
        }
        if (g_disc.hit_i < g_disc.n_hits && g_disc.found < 4) {
            int i = g_disc.hit_i++;
            disc_spawn_probe(g_disc.hit[i], g_disc.defport, NULL);
            return;
        }
        g_disc.phase = D_FINISH;
        return;
    case D_FINISH:
        disc_finish();
        return;
    default:
        g_disc.phase = D_IDLE;
        return;
    }
}

static int rb_start_discovery(char ***out, int *n)
{
    *out = NULL;
    *n = 0;                            /* 受理即返: 空 as; 结果经 discovered_printers notify */
    if (g_wj.phase != WJ_IDLE || prn_state_is("joining") || prn_state_is("discovering"))
        return PRN_BE_BUSY;

    const char *direct = prn_config_get("printer_ip");
    if (direct[0] == 0 && !prn_state_is("joined") && !prn_state_is("ready")) {
        LOGW("real: discovery needs joined state or printer_ip direct mode");
        return PRN_BE_FAILED;
    }

    prn_set_connection_state("discovering");
    prn_discovered_clear();
    eps_clear();
    memset(&g_disc, 0, sizeof g_disc);
    g_disc.t0 = now_ms();
    g_disc.defport = cfg_port();
    g_disc.port = g_disc.defport ? g_disc.defport : IPP_DEF_PORT;

    /* L1 端点表: printer_ip 缓存(逗号分隔 host[:port]; 上次发现学得回写) */
    if (direct[0]) {
        char list[512];
        snprintf(list, sizeof list, "%s", direct);
        char *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            char host[80];
            int p = parse_hostport(tok, host, sizeof host);
            if (!host[0] || g_disc.n_eps >= 8) continue;
            snprintf(g_disc.ep_ip[g_disc.n_eps], sizeof g_disc.ep_ip[0], "%s", host);
            g_disc.ep_port[g_disc.n_eps] = p ? p : g_disc.defport;
            g_disc.n_eps++;
        }
    }
    g_disc.phase = (g_disc.n_eps > 0) ? D_L1 : D_MDNS;
    LOGI("real: discovery 受理(L1 %d 端点 → L2 mDNS → L3 网关 → L4 /24, 总预算 %ds; "
         "完成=discovered_printers notify + 离开 discovering)",
         g_disc.n_eps, DISC_BUDGET_MS / 1000);
    return 0;
}

/* ---------------- 发现辅助: 网关解析 + /24 端口扫描 ---------------- */

/* /proc/net/route 解析默认路由网关(HexLE)。打印机热点里网关=打印机本身
 * (NOTES §15: 10.192.168.1 实测; DHCP siaddr 在该网络与网关同体)。
 * 返回 0=成功(gw 填点分串), -1=无默认路由。 */
static int default_gw(char *gw, size_t cap)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char iface[32];
        unsigned dest, gwhex;
        if (sscanf(line, "%31s %x %x", iface, &dest, &gwhex) != 3) continue;
        if (dest != 0 || gwhex == 0) continue;          /* 只看默认路由 */
        struct in_addr a = { .s_addr = gwhex };        /* 字段为小端 hex, 恰=network 序 */
        snprintf(gw, cap, "%s", inet_ntoa(a));
        found = 1;
        LOGI("real: 默认路由网关 %s (iface=%s, 热点网段打印机=网关)", gw, iface);
        break;
    }
    fclose(f);
    return found ? 0 : -1;
}

/* wlan0 的 IPv4 地址/掩码(ioctl, 无 getifaddrs 依赖); 0=成功 */
static int if_addr_netmask(const char *ifname, struct in_addr *addr, struct in_addr *mask)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq fr;
    memset(&fr, 0, sizeof fr);
    snprintf(fr.ifr_name, sizeof fr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFADDR, &fr) != 0) { close(s); return -1; }
    *addr = ((struct sockaddr_in *)&fr.ifr_addr)->sin_addr;
    if (ioctl(s, SIOCGIFNETMASK, &fr) != 0) { close(s); return -1; }
    *mask = ((struct sockaddr_in *)&fr.ifr_netmask)->sin_addr;
    close(s);
    return 0;
}

/* 对 base(+1..+254) 的 :port 做非阻塞 connect 扫描(48 个/批 × 800ms, 全程 ~5s);
 * 命中的地址(网络序)写入 hits, 返回命中数。(已连接 socket 恒可写, 无需特判) */
static int tcp_scan_subnet(struct in_addr base, int port, in_addr_t *hits, int max_hits)
{
    int total = 0;
    for (int start = 1; start <= 254 && total < max_hits; start += 48) {
        int fds[48];
        in_addr_t addrs[48];
        int nf = 0, maxfd = 0;
        fd_set wf;
        FD_ZERO(&wf);
        for (int h = start; h < start + 48 && h <= 254 && nf < 48; h++) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons((unsigned short)port);
            sa.sin_addr.s_addr = base.s_addr + htonl((unsigned)h);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
            if (r != 0 && errno != EINPROGRESS) { close(fd); continue; }
            fds[nf] = fd;
            addrs[nf] = sa.sin_addr.s_addr;
            FD_SET(fd, &wf);
            nf++;
            if (fd > maxfd) maxfd = fd;
        }
        if (nf > 0) {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 800000 };   /* 冷启动容忍: 闲置打印机 TCP accept 可超 350ms */
            select(maxfd + 1, NULL, &wf, NULL, &tv);
            for (int i = 0; i < nf && total < max_hits; i++) {
                if (!FD_ISSET(fds[i], &wf)) continue;   /* 未就绪=该址无服务 */
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) hits[total++] = addrs[i];
            }
            for (int i = 0; i < nf; i++) close(fds[i]);
        }
    }
    return total;
}

/* 能力探测 + 格式协商(INTERFACE.md §2b): image/jpeg 直发优先;
 * 仅栅格格式(image/pwg-raster 等)→failed"该打印机需栅格后端, 暂不支持" */
static const char *negotiate_format(const ipp_caps_t *caps)
{
    int jpeg = 0, raster = 0;
    for (int i = 0; i < caps->n_formats; i++) {
        if (!strcmp(caps->document_formats[i], "image/jpeg")) jpeg = 1;
        else if (!strcmp(caps->document_formats[i], "image/pwg-raster") ||
                 !strcmp(caps->document_formats[i], "image/urf") ||
                 !strcmp(caps->document_formats[i], "cups-raster")) raster = 1;
    }
    if (jpeg) return "image/jpeg";
    if (raster) {
        be_err("该打印机需栅格后端(仅支持 %s 等栅格格式), 暂不支持",
               caps->n_formats ? caps->document_formats[0] : "raster");
        LOGE("real: 格式协商失败 —— %s", g_be_err);
        return NULL;
    }
    be_err("document-format-supported(%d 项)不含 image/jpeg", caps->n_formats);
    LOGE("real: 格式协商失败 —— %s", g_be_err);
    return NULL;
}

static int rb_select_printer(const char *name)
{
    if (!name || !*name) return PRN_BE_PARAM;
    be_err("");
    prn_state *st = prn_state_get();
    if (st->discovered_n == 0) { LOGW("real: select before discovery"); return PRN_BE_FAILED; }

    char el_name[PRN_SLEN];
    /* 侧表优先(带端口/路径); 直连 IP 未走发现时兜底按 IP 现探 */
    const prn_ep_t *ep = eps_find(name, el_name, sizeof el_name);
    const char *el_ip = ep ? ep->ip : NULL;
    int el_port = ep ? ep->port : 0;
    const char *el_path = ep ? ep->path : NULL;
    if (!el_ip) {
        /* name 形如 IP(可带 :port) 且不在侧表: 直接按 IP 选中(探活即准) */
        char host[80];
        int port = parse_hostport(name, host, sizeof host);
        struct in_addr dummy;
        if (host[0] && inet_pton(AF_INET, host, &dummy) == 1) {
            el_ip = host;
            el_port = port ? port : cfg_port();
            el_path = NULL;
            snprintf(el_name, sizeof el_name, "%s", host);
        }
    }
    if (!el_ip) { LOGW("real: '%s' not in discovered list", name); return PRN_BE_PARAM; }
    if (!prn_state_is("joined") && !prn_state_is("ready")) {
        LOGW("real: select in state %s", st->connection_state);
        return PRN_BE_FAILED;
    }

    /* 能力探测(一切之源): requested all + media-col-database, 摊平解析入 caps
     * (冷启动容忍档 5s/12s: 闲置打印机 7KB 应答可拖过 5s——2026-09-26 真机
     *  紧档误杀实证"已收 7220 字节超时"; 与发现子进程同档) */
    prn_caps_invalidate();                             /* 旧打印机能力先失效 */
    ipp_set_path(el_path && el_path[0] ? el_path : NULL);
    ipp_caps_t caps;
    ipp_set_timeouts(5000, 12000);
    int e = ipp_get_printer_attributes(el_ip, el_port, &caps);
    ipp_set_timeouts(0, 0);
    if (e != IPP_OK) {
        LOGE("real: probe %s failed: %s (%s)", el_ip, ipp_strerror(e), ipp_last_detail());
        be_err("能力探测失败: %s", ipp_last_detail());
        return PRN_BE_FAILED;
    }

    /* 格式协商: 决定 document_format(只读属性反映实际用值) */
    const char *docfmt = negotiate_format(&caps);
    if (!docfmt) return PRN_BE_FAILED;

    snprintf(g_ip, sizeof g_ip, "%.63s", el_ip);
    g_port = el_port > 0 ? el_port : IPP_DEF_PORT;
    g_caps = caps;
    prn_set_printer_name(el_name);
    prn_set_printer_ip(el_ip);
    prn_set_printer_state(caps.printer_state == 4 ? "busy"
                         : caps.printer_state == 5 ? "error" : "idle");
    prn_caps_publish(&g_caps, docfmt);                  /* 能力+默认值发布(逐属性 notify) */
    prn_set_connection_state("ready");
    LOGI("real: selected '%s' @ %s:%d path=%s (docfmt=%s media=%d copies_max=%d res=%d)",
         el_name, el_ip, g_port, ipp_get_path(), docfmt,
         caps.n_media, caps.copies_max, caps.res_default_x);
    return 0;
}

static int suffix_is(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

static int rb_photo_list(const char *path, char ***out, int *n)
{
    const char *dir = (path && *path) ? path : prn_config_get("photo_root");
    if (!dir || !*dir) dir = "/mnt/media_runtime";     /* 相机默认媒体目录 */
    DIR *d = opendir(dir);
    if (!d) { LOGW("real: opendir '%s' failed", dir); return PRN_BE_PARAM; }
    char **items = calloc(RJ_MAX_FILES + 1, sizeof(char *));
    if (!items) { closedir(d); return PRN_BE_FAILED; }
    int cnt = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && cnt < RJ_MAX_FILES) {
        if (de->d_name[0] == '.') continue;
        /* 列"可打印"格式: jpg/jpeg 直发; .3fr 内嵌 preview; .heic/.heif heif_thumb(§26) */
        if (suffix_is(de->d_name, ".jpg") || suffix_is(de->d_name, ".jpeg")
            || suffix_is(de->d_name, ".3fr")
            || suffix_is(de->d_name, ".heic") || suffix_is(de->d_name, ".heif")) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s/%s", dir, de->d_name);
            items[cnt++] = strdup(buf);
        }
    }
    closedir(d);
    *out = items;
    *n = cnt;
    LOGI("real: photo_list '%s' -> %d", dir, cnt);
    return 0;
}

/* 打印参数解析: opts(NULL=全用属性) 与属性归并为生效值, 并按能力校验。
 * 出参: copies/quality_enum/media/color/scaling + 纸张基准像素。
 * 返回 0=ok, PRN_BE_PARAM=越能力, PRN_BE_FAILED=能力未就绪。 */
static int resolve_job_opts(const prn_job_opts_t *opts, int *copies, int *quality_enum,
                            const char **media, const char **color, const char **scaling,
                            int *paper_w, int *paper_h)
{
    prn_state *st = prn_state_get();
    be_err("");
    if (!st->caps_ready) {
        LOGW("real: caps not ready (select_printer first)");
        be_err("打印机能力未就绪(先 Select_printer)");
        return PRN_BE_FAILED;
    }

    *copies = (opts && opts->copies > 0) ? opts->copies : st->copies;
    if (*copies < 1 || *copies > st->copies_max) {
        LOGW("real: copies=%d 越界(1..%d)", *copies, st->copies_max);
        be_err("copies %d 超出该打印机上限 %d", *copies, st->copies_max);
        return PRN_BE_PARAM;
    }

    *media = (opts && opts->media && opts->media[0]) ? opts->media : st->media;
    if (!prn_list_contains(&st->sup_media, *media)) {
        LOGW("real: media '%s' 不在该打印机 supported_media", *media);
        be_err("media '%s' 不在 supported_media", *media);
        return PRN_BE_PARAM;
    }
    int w, h;
    int pe = ipp_media_target_px(&g_caps, *media, &w, &h);   /* 尺寸自计算 */
    if (pe == -2) {
        LOGW("real: media '%s' 无物理尺寸信息(media-col 缺失)", *media);
        be_err("media '%s' 无尺寸信息(打印机未通告 media-col)", *media);
        return PRN_BE_PARAM;
    }
    *paper_w = w;
    *paper_h = h;

    const char *q = (opts && opts->print_quality && opts->print_quality[0])
                    ? opts->print_quality : st->print_quality;
    *quality_enum = ipp_quality_enum(q);
    if (!*quality_enum || !prn_list_contains(&st->sup_qualities, q)) {
        LOGW("real: print_quality '%s' 不在 supported_qualities", q);
        be_err("print_quality '%s' 不在 supported_qualities", q);
        return PRN_BE_PARAM;
    }

    *color = (opts && opts->color_mode && opts->color_mode[0])
             ? opts->color_mode : st->color_mode;
    if (!prn_list_contains(&st->sup_color_modes, *color)) {
        LOGW("real: color_mode '%s' 不在 supported_color_modes", *color);
        be_err("color_mode '%s' 不在 supported_color_modes", *color);
        return PRN_BE_PARAM;
    }

    *scaling = (opts && opts->print_scaling && opts->print_scaling[0])
               ? opts->print_scaling : NULL;
    if (*scaling && g_caps.n_scalings > 0) {           /* 打印机通告列表时才校验 */
        int hit = 0;
        for (int i = 0; i < g_caps.n_scalings; i++)
            if (!strcmp(g_caps.print_scalings[i], *scaling)) { hit = 1; break; }
        if (!hit) {
            LOGW("real: print-scaling '%s' 不在 print-scaling-supported", *scaling);
            be_err("print-scaling '%s' 不在 print-scaling-supported", *scaling);
            return PRN_BE_PARAM;
        }
    }
    return 0;
}

static int rb_start_print_job(char **paths, int npaths, const prn_job_opts_t *opts,
                              char **out_job_id)
{
    *out_job_id = NULL;
    if (!paths || npaths <= 0) return PRN_BE_PARAM;
    if (!prn_state_is("ready")) {
        LOGW("real: print in state %s", prn_state_get()->connection_state);
        return PRN_BE_FAILED;
    }
    if (prn_job_active()) return PRN_BE_BUSY;
    if (!g_ip[0]) {
        LOGW("real: no printer selected (select_printer first)");
        return PRN_BE_FAILED;
    }

    /* 生效参数(能力校验+尺寸自计算): media 物理尺寸 × printer-resolution-default */
    int copies, quality_enum, paper_w, paper_h;
    const char *media, *color, *scaling;
    int rc = resolve_job_opts(opts, &copies, &quality_enum, &media, &color, &scaling,
                              &paper_w, &paper_h);
    if (rc != 0) return rc;

    if (prn_mkdir_p(X2P_TMPDIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_TMPDIR);
        return PRN_BE_FAILED;
    }

    rj_free_files();
    g_rj.jobno++;                                      /* 先定号: tmps 命名/job-N/rx 帧同源 */
    for (int i = 0; i < npaths && i < RJ_MAX_FILES; i++)
        g_rj.files[g_rj.nfiles++] = strdup(paths[i]);
    g_rj.copies = copies;
    g_rj.done_jobs = 0;
    g_rj.quality_enum = quality_enum;
    snprintf(g_rj.media, sizeof g_rj.media, "%s", media);
    snprintf(g_rj.color, sizeof g_rj.color, "%s", color);
    snprintf(g_rj.scaling, sizeof g_rj.scaling, "%s", scaling ? scaling : "");
    g_rj.paper_w = paper_w;
    g_rj.paper_h = paper_h;
    opts_to_edit(opts, &g_rj.edit);                       /* §18 裁切/旋转定形 */
    for (int i = 0; i < RJ_MAX_FILES; i++) {
        g_rj.ipp_ids[i] = 0;
        snprintf(g_rj.tmps[i], sizeof g_rj.tmps[0], "%s/prep_%d_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), g_rj.jobno, i);
    }

    prn_set_job_progress(0);
    rj_set_status("preparing", NULL);                  /* propertiesChanged + rx(ev=1) */
    prn_set_printer_state("busy");
    g_rj.phase = RJ_PREP;
    g_rj.t0 = now_ms();
    g_rj.last_poll = 0;

    char id[32];
    snprintf(id, sizeof id, "job-%d", g_rj.jobno);
    *out_job_id = strdup(id);
    LOGI("real: print job %s accepted (%d file(s), copies=%d/%d, media=%s, target=%dx%d, "
         "quality=%d, color=%s%s%s, rot=%d°, crop=%.3f,%.3f,%.3f,%.3f)", id,
         g_rj.nfiles, copies, prn_state_get()->copies_max, media, paper_w, paper_h,
         quality_enum, color, scaling ? ", scaling=" : "", scaling ? scaling : "",
         g_rj.edit.rotation, g_rj.edit.crop_x, g_rj.edit.crop_y,
         g_rj.edit.crop_w, g_rj.edit.crop_h);
    return 0;
}

static int rb_cancel_job(void)
{
    if (!prn_job_active()) {
        be_err("无进行中的打印任务(当前 job_status=%s)", prn_state_get()->job_status);
        LOGW("real: cancel_job: %s", prn_be_last_error());
        return 0;                                      /* b=false + 错误说明(be_err/日志) */
    }
    /* P0: 本地立即取消(方法即返); IPP Cancel-Job 下发+有界确认环(≤3s)整体
     * 在子进程内(不再叠加在主循环/其它长步骤上), tick_cancel() 收尸后发
     * rx(ev=2) 帧与日志。preparing 阶段无远端任务, 本地取消即终态。 */
    g_rj.phase = RJ_CANCELED;
    g_rj.t0 = now_ms();
    rj_set_status("canceled", NULL);
    prn_set_printer_state("idle");
    cancel_job_async();
    return 1;
}

/* ---------------- §18 预览/裁切/旋转(GUI 编辑页三方法) ---------------- */

#if defined(__ANDROID__)
#define X2P_PREVIEW_DIR "/data/x2print/preview"      /* 相机: 与 tmp/preview 同级 */
#else
#define X2P_PREVIEW_DIR "/tmp/x2print/preview"
#endif

/* FNV-1a 64bit: 缓存文件名的路径哈希(不把长路径进文件名) */
static unsigned long long path_hash64(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* opts → imgprep 编辑参数(adaptor 已保证数值合法; NULL=全幅+EXIF 自动) */
static void opts_to_edit(const prn_job_opts_t *o, imgprep_edit_t *e)
{
    if (o) {
        e->crop_x = o->crop_x; e->crop_y = o->crop_y;
        e->crop_w = o->crop_w; e->crop_h = o->crop_h;
        e->rotation = o->rotation;
    } else {
        e->crop_x = 0; e->crop_y = 0; e->crop_w = 1; e->crop_h = 1;
        e->rotation = -1;
    }
}

/* mtime 比较: a 晚于或等于 b → 1(缓存比源新=命中) */
static int mtime_ge(const struct stat *a, const struct stat *b)
{
    if (a->st_mtim.tv_sec != b->st_mtim.tv_sec)
        return a->st_mtim.tv_sec > b->st_mtim.tv_sec;
    return a->st_mtim.tv_nsec >= b->st_mtim.tv_nsec;
}

/* doGet_photo_info: 头部探测, 不解码像素 */
static int rb_photo_info(const char *path, prn_photo_info_t *out)
{
    if (!path || !*path) {
        be_err("路径为空");
        return PRN_BE_PARAM;
    }
    /* §26: HEIC/HEIF → heif_thumb 提取缩略图到临时件, probe 临时件 */
    char heic_tmp[192] = "";
    const char *probe_path = path;
    if (suffix_is(path, ".heic") || suffix_is(path, ".heif")) {
        snprintf(heic_tmp, sizeof heic_tmp, "%s/info_%d.jpg",
                 X2P_TMPDIR, (int)getpid());
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s 2>/dev/null",
                 X2P_ROOT, path, heic_tmp);
        if (system(hcmd) != 0) {
            remove(heic_tmp);
            be_err("%s: HEIC 解码失败", path);
            return PRN_BE_FAILED;
        }
        probe_path = heic_tmp;
    }
    imgprep_info_t ii;
    int e = imgprep_probe(probe_path, &ii);
    if (heic_tmp[0]) { remove(heic_tmp); probe_path = path; }
    if (e != IMGPREP_OK) {
        be_err("%s: %s", path, imgprep_strerror(e));
        LOGW("real: photo_info %s 失败: %s", path, imgprep_strerror(e));
        return e == IMGPREP_E_IO ? PRN_BE_PARAM : PRN_BE_FAILED;
    }
    out->width = ii.width;
    out->height = ii.height;
    out->orientation = ii.orientation;
    out->rotated_width = ii.rotated_width;
    out->rotated_height = ii.rotated_height;
    out->has_jpeg = ii.has_jpeg;
    out->is_3fr = ii.is_3fr;
    out->file_size = ii.file_size;
    LOGI("real: photo_info %s -> %dx%d orient=%d rotated=%dx%d %s(%ld B)",
         path, ii.width, ii.height, ii.orientation, ii.rotated_width,
         ii.rotated_height, ii.is_3fr ? "[3FR] " : "", ii.file_size);
    return 0;
}

/* doGet_photo_preview: 快速预览图(网格缩略图)。
 * - FAST 管线(无锐化), 长边 ≤ max(默认 1024), EXIF 自动旋转(编辑参数不参与
 *   —— 编辑效果走 doPreview_print);
 * - 3FR 感知 = 内嵌 preview 提取(fseek/fread 命中区段, 不整文件载入);
 * - 缓存: <preview>/<路径哈希>_<w>x<h>.jpg, 源 mtime 校验(源变更自动失效)。 */
static int rb_photo_preview(const char *path, int max_w, int max_h, char **out_path)
{
    *out_path = NULL;
    if (!path || !*path) {
        be_err("路径为空");
        return PRN_BE_PARAM;
    }
    if (max_w <= 0) max_w = 1024;                    /* 默认; 0/负=缺省 */
    if (max_h <= 0) max_h = 1024;
    if (max_w > 4096) max_w = 4096;                  /* 防滥用 */
    if (max_h > 4096) max_h = 4096;

    struct stat sst;
    if (stat(path, &sst) != 0) {
        be_err("文件不存在: %s", path);
        return PRN_BE_PARAM;
    }
    /* 源头探测: 旋转后逻辑尺寸(定目标宽高比) */
    prn_photo_info_t pi;
    int rc = rb_photo_info(path, &pi);
    if (rc != 0) return rc;

    double sc = 1.0;
    if (pi.rotated_width > max_w || pi.rotated_height > max_h)
        sc = (double)max_w / pi.rotated_width < (double)max_h / pi.rotated_height
             ? (double)max_w / pi.rotated_width : (double)max_h / pi.rotated_height;
    int tw = (int)(pi.rotated_width * sc + 0.5);
    int th = (int)(pi.rotated_height * sc + 0.5);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    if (prn_mkdir_p(X2P_PREVIEW_DIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_PREVIEW_DIR);
        return PRN_BE_FAILED;
    }
    char cache[256];
    snprintf(cache, sizeof cache, "%s/%016llx_%dx%d.jpg", X2P_PREVIEW_DIR,
             path_hash64(path), tw, th);

    /* 缓存命中: 输出存在且不早于源 mtime */
    struct stat cst;
    if (stat(cache, &cst) == 0 && cst.st_size > 0 && mtime_ge(&cst, &sst)) {
        LOGI("real: photo_preview 缓存命中 %s (%dx%d)", cache, tw, th);
        *out_path = strdup(cache);
        return 0;
    }

    long long t0 = now_ms();
    int e;
    if (pi.is_3fr) {
        e = imgprep_from_3fr_edit(path, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
    } else if (suffix_is(path, ".heic") || suffix_is(path, ".heif")) {
        /* §26: HEIC → heif_thumb 缩略图 → FAST 管线 */
        char htmp[192];
        snprintf(htmp, sizeof htmp, "%s/pv_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s 2>/dev/null",
                 X2P_ROOT, path, htmp);
        if (system(hcmd) != 0) { remove(htmp); be_err("HEIC 解码失败"); return PRN_BE_FAILED; }
        e = imgprep_prepare_edit(htmp, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
        remove(htmp);
    } else {
        e = imgprep_prepare_edit(path, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
    }
    if (e != IMGPREP_OK) {
        be_err("%s: %s", path, imgprep_strerror(e));
        LOGW("real: photo_preview %s 失败: %s", path, imgprep_strerror(e));
        remove(cache);                                 /* 半成品不残留 */
        return e == IMGPREP_E_IO ? PRN_BE_PARAM : PRN_BE_FAILED;
    }
    LOGI("real: photo_preview %s -> %s (%dx%d, %.0f ms)", path, cache, tw, th,
         (now_ms() - t0) / 1.0);
    *out_path = strdup(cache);
    return 0;
}

/* doPreview_print: WYSIWYG —— 与打印完全同管线(同 crop/rotation/media/scaling/
 * HQ/锐化/编码, rj_prep_file 共用), 输出落 <preview>/job_<n>_preview.jpg
 * (n 服务级递增, 环形保留最近 8 份防目录无限膨胀)。300dpi 尺寸信息从文件读。 */
#define PV_RING_N 8
static char g_pv_ring[PV_RING_N][192];
static int  g_pv_ring_head;                         /* 下一个写入槽 */
static int  g_pv_ring_len;
static int  g_pvno;

static int rb_preview_print(const char *path, const prn_job_opts_t *opts,
                            char **out_path)
{
    *out_path = NULL;
    if (!path || !*path) {
        be_err("路径为空");
        return PRN_BE_PARAM;
    }
    if (prn_job_active()) {
        be_err("打印任务进行中(%s), 稍后再预览", prn_state_get()->job_status);
        return PRN_BE_BUSY;
    }
    if (access(path, R_OK) != 0) {
        be_err("文件不存在: %s", path);
        return PRN_BE_PARAM;
    }

    /* 生效参数(能力校验+尺寸自计算, 与 Start_print_job 同闸) */
    int copies, quality_enum, paper_w, paper_h;
    const char *media, *color, *scaling;
    int rc = resolve_job_opts(opts, &copies, &quality_enum, &media, &color,
                              &scaling, &paper_w, &paper_h);
    if (rc != 0) return rc;                          /* be_err 已填 */

    /* 编辑参数: 按**显式旋转后**的尺寸做最小裁切校验(EXIF 自动=probe 的 rotated) */
    imgprep_edit_t edit;
    opts_to_edit(opts, &edit);
    int ow, oh;
    if (oriented_size_ex(path, &edit, &ow, &oh) != 0) {
        if (suffix_is(path, ".3fr")) {
            /* 3FR: 头部在 preview 里, oriented_size_ex 读不了 → 用 probe 结果 */
            prn_photo_info_t pi;
            if (rb_photo_info(path, &pi) != 0) return PRN_BE_FAILED;
            int swap = (edit.rotation >= 0 && (edit.rotation / 90) % 2 == 1);
            ow = swap ? pi.height : pi.width;
            oh = swap ? pi.width : pi.height;
        } else {
            be_err("无法读取图像尺寸: %s", path);
            return PRN_BE_FAILED;
        }
    }
    if (imgprep_edit_validate(&edit, ow, oh) != IMGPREP_OK) {
        be_err("裁切/旋转参数非法(裁切窗两维需 ≥%d 源像素)", IMGPREP_MIN_CROP_PX);
        LOGW("real: preview_print %s edit 非法(crop=%.3f,%.3f,%.3f,%.3f rot=%d, 源 %dx%d)",
             path, edit.crop_x, edit.crop_y, edit.crop_w, edit.crop_h, edit.rotation, ow, oh);
        return PRN_BE_PARAM;
    }

    if (prn_mkdir_p(X2P_PREVIEW_DIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_PREVIEW_DIR);
        return PRN_BE_FAILED;
    }
    char out[192];
    snprintf(out, sizeof out, "%s/job_%d_preview.jpg", X2P_PREVIEW_DIR, ++g_pvno);
    /* 环形淘汰: 满 8 份时删最老的一份 */
    if (g_pv_ring_len == PV_RING_N) {
        remove(g_pv_ring[g_pv_ring_head]);
        g_pv_ring_len--;
    }
    snprintf(g_pv_ring[g_pv_ring_head], sizeof g_pv_ring[0], "%s", out);
    g_pv_ring_head = (g_pv_ring_head + 1) % PV_RING_N;
    if (g_pv_ring_len < PV_RING_N) g_pv_ring_len++;

    imgprep_report_t rep;
    long long t0 = now_ms();
    if (rj_prep_file(path, out, paper_w, paper_h, &edit, &rep) != 0) {
        remove(out);
        g_pv_ring_len--;                              /* 槽作废 */
        return PRN_BE_FAILED;                         /* be_err 已填(rj_prep_file) */
    }
    LOGI("real: preview_print job_%d %s -> %dx%d (%ld B, rot=%d°, crop=%d,%d %dx%d, "
         "media=%s, %.0f ms)", g_pvno, path, rep.out_w, rep.out_h, rep.out_bytes,
         rep.rotation, rep.ucrop_x, rep.ucrop_y, rep.ucrop_w, rep.ucrop_h, media,
         (now_ms() - t0) / 1.0);
    *out_path = strdup(out);
    return 0;
}

/* ---------------- 闲置看门狗(INTERFACE.md §4b, GUI 崩溃/忘了断开的兜底) ----------------
 * joined/ready 且无打印任务且无业务方法调用(含 Properties.Set; Get/GetAll 轮询
 * 不算)超过 idle_restore_min 分钟 → 自动执行与 Disconnect_and_restore 相同的
 * 恢复路径(propertiesChanged connection_state→disconnected 广播可见)。
 * §20 扩展: Scan_wifi 后芯片保持可用而用户放弃连接(仍 disconnected)同样计时
 * (g_scan_kept), 到点收尾拆芯片/还原电源(扫描让出过 AP 的回原状)。
 * config idle_restore_min: 缺省 5, 0=禁用。join/discovering 进行中不触发;
 * 活跃操作(任务/阻塞扫描)期间由各自超时兜底, 不受看门狗打断。 */
static void tick_idle_watchdog(void)
{
    static long long armed_at = 0;                     /* 上次 armed 时刻(防重复日志) */
    static int prev_active = 0;                        /* 上一 tick 是否任务进行中 */
    int active = prn_job_active();
    if (prev_active && !active) {
        prn_adaptor_touch();                           /* 任务刚结束=最近活动, 时钟重置 */
        LOGD("real: 任务结束, 闲置时钟自此刻重算");
    }
    prev_active = active;
    const char *k = prn_config_get("idle_restore_min");
    int min = k[0] ? atoi(k) : 5;                      /* 空=缺省 5; "0"=显式禁用 */
    if (min <= 0) return;
    if (g_wj.phase != WJ_IDLE) {                         /* join/恢复进行中不触发 */
        armed_at = now_ms();
        return;
    }
    if (g_disc.phase != D_IDLE) {                        /* 发现进行中不触发 */
        armed_at = now_ms();
        return;
    }
    if (!prn_state_is("joined") && !prn_state_is("ready") && !g_scan_kept) {
        armed_at = 0;
        return;
    }
    if (active) {                                      /* 有任务: 任务超时自兜底 */
        armed_at = now_ms();
        return;
    }
    long long idle = prn_adaptor_idle_ms();
    long long limit = (long long)min * 60 * 1000;
    if (idle < limit) return;
    if (armed_at && now_ms() - armed_at < 1000) return;
    armed_at = now_ms();
    LOGW("real: 闲置看门狗触发(空闲 %.1f 分 > %d 分, 无任务无业务调用) → 自动恢复",
         idle / 60000.0, min);
    rb_disconnect_restore();                           /* 幂等恢复+disconnected 广播 */
}

static void rb_tick(void)
{
    tick_join();
    tick_scan();
    tick_discovery();
    tick_cancel();                                     /* 取消环子进程收尾(rx 帧) */
    tick_job();
    tick_idle_watchdog();
}

/* ---------------- 会话-待机防护(§19): wakelock + 息屏治理 ----------------
 * 用户口径: 打印的时候不息屏, 打印完再恢复。两层:
 *   ①底线(wifi_cam_wake_hold): 内核 wakelock 防 autosleep 挂起 —— 待机下
 *     camera-system 对 power_on_wifi「受理但不起」(§18.5 真相), 且挂起威胁
 *     STA 链路;
 *   ②显示层: 会话期临时置 screen_idle_timeout=86400(打印不息屏), 恢复完成
 *     还原原值; 原值同时落盘 /data/x2print/screen_timeout.saved —— 服务中途
 *     崩溃时由 boot_init 在下次启动还原(崩溃恢复凭据)。
 * 生命周期: join 受理(guard_on) → 恢复完成/失败回滚(guard_off, 在合法电源
 * 同步之后 —— 重拉 AP 同样需要清醒)。幂等: 未持有时 guard_off 无副作用。 */
#define SCR_SAVE_FILE "/data/x2print/screen_timeout.saved"
#define SCR_SESSION   86400                       /* 会话期息屏超时(24h≈永不) */
static int g_scr_saved = -1;                      /* 会话前原值; -1=未覆写 */

/* 前半(wakelock + 读原值落凭据) —— P0 拆两 tick: 每 tick 至多一次阻塞 D-Bus */
static void session_guard_a(void)
{
    wifi_cam_wake_hold();                         /* ①底线: 防挂起 */
    int cur = -1;
    if (prn_adaptor_screen_idle_get(&cur) == 0 && cur > 0 && cur < SCR_SESSION) {
        g_scr_saved = cur;
        FILE *f = fopen(SCR_SAVE_FILE, "w");      /* 崩溃恢复凭据 */
        if (f) { fprintf(f, "%d\n", cur); fclose(f); }
    } else if (cur >= SCR_SESSION) {
        LOGI("real: screen_idle_timeout 已 ≥%d(用户本就不息屏), 不覆写", SCR_SESSION);
    } else {
        LOGW("real: screen_idle_timeout 读取异常(%d) —— 仅 wakelock 防护", cur);
    }
}

/* 后半(覆写息屏超时) */
static void session_guard_b(void)
{
    if (g_scr_saved > 0) {
        if (prn_adaptor_screen_idle_set(SCR_SESSION) == 0)
            LOGI("real: 会话防护: screen_idle_timeout %d→%d(打印不息屏, 原值已存)",
                 g_scr_saved, SCR_SESSION);
        else
            LOGW("real: screen_idle_timeout 覆写失败 —— 仅 wakelock 防护(屏仍会息)");
    }
}

static void session_guard_off(void)
{
    if (g_scr_saved > 0) {
        if (prn_adaptor_screen_idle_set(g_scr_saved) == 0)
            LOGI("real: screen_idle_timeout 还原 %d(恢复正常息屏)", g_scr_saved);
        else
            LOGW("real: screen_idle_timeout 还原失败(原值 %d, 可经 GUI/printerctl 手动恢复)",
                 g_scr_saved);
        remove(SCR_SAVE_FILE);
        g_scr_saved = -1;
    }
    wifi_cam_wake_release();                      /* 在合法电源同步之后(§19) */
}

/* 启动钩子(main 在总线就绪后调一次): §19 崩溃残留收尾 + P1② 全残留清理。
 * ①清陈旧会话 wakelock; ②screen_timeout.saved 还原息屏原值;
 * ③wifi_cam_boot_cleanup: 陈旧 wpa_supplicant(pid 文件+pgrep 自带路径)/
 *   module.owned 凭据 rmmod/wlan0 地址 flush(仅凭据在案);
 * ④ap_was_on.saved: 上次会话让出过原生 AP 未及恢复 → 重新拉起。
 * 全程幂等+日志; WSL/无凭据时为无操作。 */
static void rb_boot_init(void)
{
    wifi_cam_wake_release();                      /* 清陈旧会话锁(幂等) */
    FILE *f = fopen(SCR_SAVE_FILE, "r");
    if (f) {
        int v = -1;
        int got = fscanf(f, "%d", &v);
        fclose(f);
        remove(SCR_SAVE_FILE);
        if (got == 1 && v > 0) {
            if (prn_adaptor_screen_idle_set(v) == 0)
                LOGI("real: 崩溃恢复: screen_idle_timeout 还原 %d", v);
            else
                LOGW("real: 崩溃恢复: screen_idle_timeout 还原失败(原值 %d)", v);
        }
    }
    /* P1②: Wi-Fi 残留清理(相机真路径; mock/桩无操作) */
    if (!wifi_is_mock())
        wifi_cam_boot_cleanup();
    /* §23: AP 原状凭据只清不重拉(恢复=关电; AP 唤醒交还用户/原厂纪律) */
    f = fopen(AP_WAS_ON_FILE, "r");
    if (f) { fclose(f); remove(AP_WAS_ON_FILE); }
}

static const prn_backend g_rb = {
    .name = "real",
    .tick = rb_tick,
    .boot_init = rb_boot_init,                        /* §19: 崩溃恢复 */
    .connect_to_ap = rb_connect_to_ap,
    .scan_wifi = rb_scan_wifi,                         /* §20 扫描选择 */
    .disconnect_restore = rb_disconnect_restore,
    .start_discovery = rb_start_discovery,
    .select_printer = rb_select_printer,
    .photo_list = rb_photo_list,
    .start_print_job = rb_start_print_job,
    .cancel_job = rb_cancel_job,
    .photo_info = rb_photo_info,                     /* §18 */
    .photo_preview = rb_photo_preview,               /* §18 */
    .preview_print = rb_preview_print,               /* §18 */
};

const prn_backend *prn_backend_real(void)
{
    x2p_netlog_set_hook(netlog_bridge);                /* 网络层日志并入服务日志 */
    x2p_netlog_set_level(X2P_LOG_INFO);
    return &g_rb;
}
