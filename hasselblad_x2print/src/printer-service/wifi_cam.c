/* wifi_cam.c — 相机侧 Wi-Fi STA 编排实现(NOTES §15 实测路径; 见 wifi_cam.h 流程)
 *
 * P0 反饿死重构(NOTES §22, 审计 §1 阻塞表逐点治理):
 *   旧 run_cmd() 同步 waitpid(命令最坏 15s/25s) + nanosleep 重试 → join 单 tick
 *   可阻塞 16-61s, 事件循环失联。现一律 fork+exec 后台子进程 + tick 内
 *   waitpid(WNOHANG) 轮询(单槽运行器 ac_*), 三台状态机(join/扫描/恢复)
 *   每次 step 至多一次非阻塞轮询 —— 主循环任何步骤间可呼吸, 断开/取消
 *   经 wifi_cam_abort() 即时 SIGKILL 在途子进程。
 *   同步 run_cmd() 仅保留给 wifi_cam_boot_cleanup()(启动一次性, 总线就绪前,
 *   无客户端在等; 各步有界+幂等)。
 *
 * psk 纪律: ssid/psk 永不出现在命令行(进程表可见)与日志; wpa.conf/psk 文件
 * fopen 直写 + chmod 0600; psk 只在 join_start 栈上停留, 不驻任何静态结构。
 *
 * 模块驻留策略(§16.4 根治, 2026-09-26 实锤): 一律不 rmmod —— rmmod 后本
 * 开机内重载撞 SDIO 枚举竞态(err=-19), 原生 power_on 的 insmod 同样会撞;
 * 模块驻留=原生 boot 状态, wms power_on 走 devreset 不依赖重载, 重启自清。
 * module.owned 凭据退役(仅残留文件清理)。恢复幂等: 重复调用安全。
 *
 * 非 __ANDROID__ 环境(WSL 联调)为桩: join_start/scan_begin 即失败(LOGW),
 * 恢复无操作, 保证 backend_real 在 WSL 可编译可运行(联调走 wifi_backend=mock)。
 */
#include "wifi_cam.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)

#define CAM_SH      "/system/bin/sh"
#define X2P_ROOT    "/data/x2print"
#define BIN_DIR     X2P_ROOT "/bin"
#define WPA_SUP     BIN_DIR "/wpa_supplicant"     /* 自带静态交叉版(NOTES §14) */
#define WPA_CLI     BIN_DIR "/wpa_cli"
#define WPA_CONF    X2P_ROOT "/wpa.conf"
#define WPA_SCAN_CONF X2P_ROOT "/wpa_scan.conf"  /* §20 扫描配置(无网络表, 0600) */
#define WPA_CTRL    X2P_ROOT "/wpasock"           /* ctrl_interface 目录 */
#define WPA_PID     X2P_ROOT "/wpa_supplicant.pid"
#define WPA_LOG     X2P_ROOT "/log/wpa.log"
#define PSK_FILE    X2P_ROOT "/psk"               /* 持久化 psk(0600, 供复连/诊断) */
#define MOD_OWNED   X2P_ROOT "/module.owned"      /* P1② 模块所有权凭据(崩溃恢复) */
#define STA_IF      "wlan0"
#define KMOD_PATH   "/system/lib/modules/bcmdhd.ko"
#define UDHCPC_SH   BIN_DIR "/udhcpc.sh"          /* DHCP 事件脚本(service.sh 装) */
#define OUT_TMP     X2P_ROOT "/tmp/cmd_out.txt"
#define WAKE_LOCK   "/sys/power/wake_lock"        /* 会话 wakelock(§19) */
#define WAKE_UNLOCK "/sys/power/wake_unlock"
#define X2P_WAKE    "x2print-session"

/* 预算(毫秒) —— 均为子进程超时上限, 不再阻塞事件循环 */
#define T_INSMOD    15000
#define T_IFUP      5000
#define T_WPA_KILL  3000
#define T_WPA_START 8000
#define T_WPA_POLL  4000     /* 单次 wpa_cli status/scan_results */
#define T_ASSOC_MS  45000    /* 关联(COMPLETED)总预算 */
#define T_DHCP      25000
#define T_TERM      4000
#define T_FLUSH     4000
#define T_DOWN      4000
#define T_RMMOD     8000
#define T_SCAN      12000    /* §20: iw dev wlan0 scan 单次预算 */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 同步命令(仅 boot_cleanup 用: 启动一次性, 无客户端在等) ----------------
 * fork+execl(sh) + waitpid 轮询, 超时 SIGKILL; 返回退出码, <0=启动失败/-2=超时 */
static int run_cmd(const char *cmd, int timeout_ms)
{
    long long t0 = now_ms();
    LOGD("wifi_cam: run(%dms): %s", timeout_ms, cmd);
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("wifi_cam: fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); }
        execl(CAM_SH, "sh", "-c", cmd, (char *)NULL);
        _exit(127);                                   /* execl 失败 */
    }
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { LOGE("waitpid: %s", strerror(errno)); return -1; }
        if (now_ms() - t0 > timeout_ms) {
            LOGW("wifi_cam: 超时(%dms), SIGKILL: %s", timeout_ms, cmd);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -2;
        }
        usleep(50 * 1000);
    }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -3;
    LOGD("wifi_cam: done rc=%d (%lldms): %s", rc, now_ms() - t0, cmd);
    return rc;
}

/* 同步取 stdout(boot_cleanup 的 pgrep 用) */
static int run_cmd_out(const char *cmd, int timeout_ms, char *out, size_t cap)
{
    remove(OUT_TMP);
    if (out && cap) out[0] = '\0';
    char full[600];
    snprintf(full, sizeof full, "%s > %s 2>/dev/null", cmd, OUT_TMP);
    int rc = run_cmd(full, timeout_ms);
    if (out && cap) {
        FILE *f = fopen(OUT_TMP, "r");
        if (f) {
            size_t n = fread(out, 1, cap - 1, f);
            out[n] = '\0';
            fclose(f);
        }
    }
    remove(OUT_TMP);
    return rc;
}

/* ---------------- 异步子进程运行器(P0 核心): fork+exec → tick 轮询 ----------------
 * 单槽(所有序列严格串行: join/扫描/恢复互斥, 由 backend 状态门保证)。
 * capture=1 时子 stdout 重定向 OUT_TMP, 收尸后 ac_out() 读回。 */
#define AC_RUNNING 1
#define AC_DONE    0

static struct {
    pid_t     pid;            /* >0 = 在跑 */
    long long t0;
    int       timeout_ms;
    int       capture;
    int       rc;             /* 收尸后退出码(127=exec 失败, -2=超时/abort 被杀) */
    char      cmd[320];       /* 日志用 */
} g_ac;

static int ac_busy(void) { return g_ac.pid > 0; }

static int ac_spawn(const char *cmd, int timeout_ms, int capture)
{
    if (ac_busy()) {
        LOGW("wifi_cam: 内部错误——前一子进程未收尸(%s), 拒绝并发", g_ac.cmd);
        return -1;
    }
    if (capture) remove(OUT_TMP);
    pid_t pid = fork();
    if (pid < 0) { LOGE("wifi_cam: fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn < 0) _exit(126);
        dup2(dn, STDERR_FILENO);
        if (capture) {
            int out = open(OUT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (out < 0) _exit(126);
            dup2(out, STDOUT_FILENO);
        } else {
            dup2(dn, STDOUT_FILENO);
        }
        execl(CAM_SH, "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    g_ac.pid = pid;
    g_ac.t0 = now_ms();
    g_ac.timeout_ms = timeout_ms;
    g_ac.capture = capture;
    g_ac.rc = 0;
    snprintf(g_ac.cmd, sizeof g_ac.cmd, "%.310s", cmd);
    LOGD("wifi_cam: spawn(%dms): %s", timeout_ms, cmd);
    return 0;
}

/* tick 轮询: 1=在跑 0=结束(g_ac.rc 有效)。超时 SIGKILL → rc=-2。非阻塞。 */
static int ac_poll(void)
{
    if (!ac_busy()) return AC_DONE;
    int st = 0;
    pid_t r = waitpid(g_ac.pid, &st, WNOHANG);
    if (r == g_ac.pid) {
        g_ac.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -3;
        LOGD("wifi_cam: reap rc=%d (%lldms): %s", g_ac.rc, now_ms() - g_ac.t0, g_ac.cmd);
        g_ac.pid = 0;
        return AC_DONE;
    }
    if (r < 0 && errno != EINTR) {
        LOGE("wifi_cam: waitpid: %s", strerror(errno));
        g_ac.pid = 0;
        g_ac.rc = -4;
        return AC_DONE;
    }
    if (now_ms() - g_ac.t0 > g_ac.timeout_ms) {
        LOGW("wifi_cam: 子进程超时(%dms), SIGKILL: %s", g_ac.timeout_ms, g_ac.cmd);
        kill(g_ac.pid, SIGKILL);
        waitpid(g_ac.pid, &st, 0);
        g_ac.pid = 0;
        g_ac.rc = -2;
        return AC_DONE;
    }
    return AC_RUNNING;
}

/* 中断: 立即 SIGKILL 在途子进程并收尸(wifi_cam_abort 用) */
static void ac_kill(void)
{
    if (!ac_busy()) return;
    LOGW("wifi_cam: abort, SIGKILL 在途子进程: %s", g_ac.cmd);
    kill(g_ac.pid, SIGKILL);
    int st;
    waitpid(g_ac.pid, &st, 0);
    g_ac.pid = 0;
    g_ac.rc = -2;
}

/* capture 型命令收尸后读回 stdout(截断 cap-1; 文件随即删除) */
static void ac_out(char *buf, size_t cap)
{
    buf[0] = 0;
    FILE *f = fopen(OUT_TMP, "r");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    remove(OUT_TMP);
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int iface_present(void)
{
    return exists("/sys/class/net/" STA_IF);
}

/* 从 "key=v" 逐行文本里提取值(wpa_cli status / ifconfig 输出等) */
static int line_get(const char *out, const char *key, char *val, size_t cap)
{
    if (!out) return 0;
    size_t kl = strlen(key);
    const char *p = out;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = 0;
            while (v[n] && v[n] != '\n' && n < cap - 1) n++;
            memcpy(val, v, n);
            val[n] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

static void mkdirs(void)
{
    mkdir(X2P_ROOT, 0755);
    mkdir(X2P_ROOT "/bin", 0755);
    mkdir(X2P_ROOT "/log", 0755);
    mkdir(X2P_ROOT "/tmp", 0755);
}

/* udhcpc 事件脚本缺失时现场生成最小版(deconfig/bound/renew) */
static void ensure_udhcpc_script(void)
{
    if (exists(UDHCPC_SH)) return;
    FILE *f = fopen(UDHCPC_SH, "w");
    if (!f) { LOGW("wifi_cam: 写 %s 失败: %s", UDHCPC_SH, strerror(errno)); return; }
    fprintf(f,
            "#!/system/bin/sh\n"
            "# x2print udhcpc 事件脚本(service.sh install 亦会部署)\n"
            "case \"$1\" in\n"
            "  deconfig) busybox ifconfig \"$interface\" 0.0.0.0 ;;\n"
            "  bound|renew) busybox ifconfig \"$interface\" \"$ip\" netmask \"$subnet\";\n"
            "               busybox route add default gw \"$router\" 2>/dev/null ;;\n"
            "esac\n");
    fclose(f);
    chmod(UDHCPC_SH, 0755);
    LOGI("wifi_cam: 已生成 %s", UDHCPC_SH);
}

/* ---------------- 会话 wakelock(NOTES §19) ----------------
 * 待机机理: 无人操作超 screen_idle_timeout 后 camera-system 释放自有 wakelock,
 * autosleep 无锁即连续挂起(adb/USB 仅瞬时唤醒) —— power_on_wifi 同步受理但
 * 异步 hostapd 序列拿不到执行(§18.5「受理但不起」真相); 挂起同样威胁 STA
 * 链路。会话生命周期内持内核命名锁防挂起; 释放时机=恢复完成(含合法电源
 * 同步之后, 由 backend_real 调), 服务启动时 release 一次清崩溃残留。 */
void wifi_cam_wake_hold(void)
{
    FILE *f = fopen(WAKE_LOCK, "w");
    if (!f) {
        LOGW("wifi_cam: wake_lock 不可用(%s) —— 会话将无待机保护", strerror(errno));
        return;
    }
    fputs(X2P_WAKE, f);
    fclose(f);
    LOGI("wifi_cam: 会话 wakelock 持有(%s)", X2P_WAKE);
}

void wifi_cam_wake_release(void)
{
    FILE *f = fopen(WAKE_UNLOCK, "w");
    if (!f)
        return;                          /* 非相机/无接口: 无声跳过(幂等) */
    fputs(X2P_WAKE, f);
    fclose(f);
    LOGI("wifi_cam: 会话 wakelock 释放(%s)", X2P_WAKE);
}

/* ---------------- 模块所有权(insmod 过才 rmmod, 且只卸一次) ----------------
 * 凭据落盘 module.owned(P1②): 进程暴毙后 g_mod_owned 内存丢失, 下次启动
 * boot_cleanup 据文件补卸; 正常恢复 rmmod 成功即删凭据。 */
static int g_mod_owned = 0;
static int g_join_fail_fast;   /* assoc 阶段事件命中快速失败(供上层文案) */      /* 我们 insmod 的 bcmdhd 且尚未 rmmod */

static void mod_owned_write(void)
{
    FILE *f = fopen(MOD_OWNED, "w");
    if (!f) { LOGW("wifi_cam: 写 %s 失败: %s(崩溃恢复将漏卸模块)", MOD_OWNED,
                   strerror(errno)); return; }
    fputs("bcmdhd\n", f);
    fclose(f);
}

/* ---------------- 中断位(P0: 断开/取消可即时中断在途步骤) ----------------
 * 实现见文件尾 wifi_cam_abort(); 各 begin 清零。 */
static int g_abort = 0;

/* ---------------- 芯片保障共享子机(join J_PREP / 扫描共用) ----------------
 * 子态: C_BEGIN(判定) → C_INSMOD(spawn+poll, ≤3 次重试) → C_GAP(3s 间隔,
 * tick 时钟计时不 nanosleep) → C_IFWAIT(等接口注册 ≤3s) → C_IFUP(+busybox
 * 回退) → 完成。0=up 1=进行中 -1=失败。 */
enum { C_OFF = 0, C_BEGIN, C_INSMOD, C_GAP, C_IFWAIT, C_IFUP, C_IFUP_BB };
static struct {
    int       sub;
    int       armed;      /* 1=当前子态子进程已 spawn 等收尸 */
    int       tries;
    long long t_sub;
} g_chip;

static void chip_begin(void)
{
    g_chip.sub = C_BEGIN;
    g_chip.armed = 0;
    g_chip.tries = 0;
}

static int chip_step(void)
{
    char cmd[160];
    switch (g_chip.sub) {
    case C_BEGIN:
        if (iface_present()) {
            LOGI("wifi_cam: " STA_IF " 已在(模块开机已载), 不取得所有权");
            g_chip.sub = C_IFUP;
            g_chip.armed = 0;
            return chip_step();
        }
        if (!exists(KMOD_PATH)) {
            LOGE("wifi_cam: %s 不存在且 wlan0 缺失", KMOD_PATH);
            return -1;
        }
        LOGI("wifi_cam: " STA_IF " 缺失 → insmod %s", KMOD_PATH);
        g_chip.sub = C_INSMOD;
        g_chip.armed = 0;
        return chip_step();
    case C_INSMOD:
        if (!g_chip.armed) {
            snprintf(cmd, sizeof cmd, "insmod %s", KMOD_PATH);
            if (ac_spawn(cmd, T_INSMOD, 0) != 0) return -1;
            g_chip.armed = 1;
            return 1;
        }
        if (g_ac.rc == 0) {
            g_chip.sub = C_IFWAIT;
            g_chip.t_sub = now_ms();
            g_chip.armed = 0;
            return chip_step();
        }
        /* rmmod 后立刻重载可能撞 SDIO 枚举竞态(dmesg: rescan_entered),
         * 间隔 3s 重试最多 3 次(§16 实测: 竞态卡死时只能重启相机) */
        if (g_chip.tries < 2) {
            g_chip.tries++;
            LOGW("wifi_cam: insmod 第 %d/3 次失败, 3s 后重试", g_chip.tries);
            g_chip.sub = C_GAP;
            g_chip.t_sub = now_ms();
            return 1;
        }
        LOGE("wifi_cam: insmod 3 次均失败(若刚 rmmod 过: SDIO 枚举竞态, 需重启相机)");
        return -1;
    case C_GAP:
        if (now_ms() - g_chip.t_sub < 3000) return 1;
        g_chip.sub = C_INSMOD;
        g_chip.armed = 0;
        return chip_step();
    case C_IFWAIT:
        if (iface_present()) {                    /* 注册异步, ≤3s */
            g_mod_owned = 1;
            mod_owned_write();
            LOGI("wifi_cam: bcmdhd 已载(所有权=我们, 凭据已落盘, 恢复时 rmmod)");
            g_chip.sub = C_IFUP;
            g_chip.armed = 0;
            return chip_step();
        }
        if (now_ms() - g_chip.t_sub > 3000) {
            LOGE("wifi_cam: insmod 后 " STA_IF " 仍未出现");
            return -1;
        }
        return 1;
    case C_IFUP:
    case C_IFUP_BB:
        if (!g_chip.armed) {
            snprintf(cmd, sizeof cmd, "%sifconfig " STA_IF " up",
                     g_chip.sub == C_IFUP_BB ? "busybox " : "");
            if (ac_spawn(cmd, T_IFUP, 0) != 0) return -1;
            g_chip.armed = 1;
            return 1;
        }
        if (g_ac.rc == 0) {
            g_chip.sub = C_OFF;
            return 0;                              /* 芯片就绪 */
        }
        if (g_chip.sub == C_IFUP) {                /* busybox 回退一次 */
            LOGW("wifi_cam: ifconfig up 失败, busybox 回退");
            g_chip.sub = C_IFUP_BB;
            g_chip.armed = 0;
            return chip_step();
        }
        LOGE("wifi_cam: ifconfig " STA_IF " up 失败");
        return -1;
    default:
        return -1;
    }
}

/* ---------------- join 阶段机 ---------------- */

enum {
    J_IDLE = 0,   /* 未在连接 */
    J_PREP,       /* 基线检测: insmod(如需) + ifconfig up(芯片子机) */
    J_SUPP,       /* 停残留 supplicant + 启动(配置已在 start 时落盘) */
    J_ASSOC,      /* 轮询 wpa_state=COMPLETED */
    J_DHCP,       /* udhcpc 取租约 */
    J_DONE        /* joined */
};
enum { S_KILL = 0, S_START, S_TAIL };      /* J_SUPP 子态 */
enum { A_POLL = 0, A_TAIL };               /* J_ASSOC 子态 */
enum { D_RUN = 0, D_IFCFG, D_IFCFG_FAIL }; /* J_DHCP 子态 */

static struct {
    int        phase, sub;
    int        armed;                 /* 1=当前子态子进程已 spawn 等收尸 */
    long long  t_phase;               /* 当前阶段起始时刻 */
    long long  t_join;                /* join_start 时刻 */
    long long  last_poll;             /* 上次 wpa_cli 轮询 */
    char       last_state[32];        /* 上次 wpa_state(变更才 LOGI) */
} g_j;

static const char *phase_names[] = {
    "idle", "prep(insmod/ifup)", "start-supplicant",
    "assoc(wait COMPLETED)", "dhcp(udhcpc)", "joined",
};

const char *wifi_cam_join_phase(void)
{
    return phase_names[g_j.phase >= 0 && g_j.phase <= J_DONE ? g_j.phase : J_IDLE];
}

static void j_phase(int p)
{
    g_j.phase = p;
    g_j.sub = 0;
    g_j.armed = 0;
    g_j.t_phase = now_ms();
    LOGI("wifi_cam: 阶段 → %s", phase_names[p]);
}

static void j_sub(int s)
{
    g_j.sub = s;
    g_j.armed = 0;
}

/* 写 wpa.conf(0600) —— psk 只经此函数文件化, 不进命令行/日志/静态结构。
 * §20: psk 空串=开放网络(key_mgmt=NONE, 无 psk 行; GUI 在列表选择 open 网络时
 * 免密直连), 此时 PSK_FILE 不落。 */
static int write_wpa_conf(const char *ssid, const char *psk)
{
    FILE *f = fopen(WPA_CONF, "w");
    if (!f) {
        LOGE("wifi_cam: 写 %s 失败: %s", WPA_CONF, strerror(errno));
        return -1;
    }
    if (psk[0]) {
        fprintf(f,
                "# x2print wpa_supplicant 配置(wifi_cam 自动生成, 0600)\n"
                "ctrl_interface=%s\n"
                "ap_scan=1\n"
                "network={\n"
                "  ssid=\"%s\"\n"
                "  psk=\"%s\"\n"
                "  key_mgmt=WPA-PSK\n"
                "  proto=RSN\n"
                "  pairwise=CCMP\n"
                "  group=CCMP\n"
                "  priority=10\n"
                "}\n",
                WPA_CTRL, ssid, psk);
    } else {
        fprintf(f,
                "# x2print wpa_supplicant 配置(wifi_cam 自动生成, 0600, 开放网络)\n"
                "ctrl_interface=%s\n"
                "ap_scan=1\n"
                "network={\n"
                "  ssid=\"%s\"\n"
                "  key_mgmt=NONE\n"
                "  priority=10\n"
                "}\n",
                WPA_CTRL, ssid);
    }
    fclose(f);
    chmod(WPA_CONF, 0600);

    if (!psk[0])
        return 0;                        /* 开放网络: 无 psk 可落 */
    f = fopen(PSK_FILE, "w");            /* psk 单独持久化(复连/诊断), 0600 */
    if (f) {
        fprintf(f, "%s\n", psk);
        fclose(f);
        chmod(PSK_FILE, 0600);
    } else {
        LOGW("wifi_cam: 写 %s 失败: %s(不影响本次连接)", PSK_FILE, strerror(errno));
    }
    return 0;
}

int wifi_cam_join_start(const char *ssid, const char *psk)
{
    /* §20: psk 空串=开放网络(key_mgmt=NONE); 1..7 位仍拒绝(半截密码必错) */
    if (!ssid || !ssid[0] || !psk || (psk[0] && strlen(psk) < 8)) {
        LOGE("wifi_cam: ssid 空/psk 非法(1..7 位), 拒绝");
        return -1;
    }
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE) {
        LOGW("wifi_cam: join 进行中(%s), 拒绝重复 start", wifi_cam_join_phase());
        return -1;
    }
    if (!exists(WPA_SUP) || !exists(WPA_CLI)) {
        LOGE("wifi_cam: 缺 %s / %s(先 service.sh install)", WPA_SUP, WPA_CLI);
        return -1;
    }
    if (strchr(ssid, '"') || strchr(psk, '"') || strchr(ssid, '\n') || strchr(psk, '\n')) {
        LOGE("wifi_cam: ssid/psk 含引号/换行, 拒绝写入配置");
        return -1;
    }
    mkdirs();
    if (write_wpa_conf(ssid, psk) != 0)     /* psk 就此离开内存 */
        return -1;

    wifi_cam_wake_hold();                   /* §19: 受理即持锁, 会话全程防待机 */

    g_abort = 0;
    g_j.last_state[0] = '\0';
    g_j.t_join = now_ms();
    chip_begin();
    j_phase(J_PREP);
    LOGI("wifi_cam: join 受理 ssid=%s (%s)", ssid, psk[0] ? "psk 已落盘 0600" : "开放网络免密");
    return 0;
}

static int step_supp(void)
{
    char cmd[320];
    switch (g_j.sub) {
    case S_KILL:
        /* 残留清理(上次异常退出的 supplicant/socket) */
        if (!g_j.armed) {
            snprintf(cmd, sizeof cmd,
                     "killall wpa_supplicant 2>/dev/null; rm -rf %s %s", WPA_CTRL, WPA_PID);
            if (ac_spawn(cmd, T_WPA_KILL, 0) != 0) return -1;
            g_j.armed = 1;
            return 1;
        }
        ensure_udhcpc_script();
        /* 实测参数(NOTES §15): -B -Dnl80211 -i wlan0 -c conf */
        remove(WPA_LOG);                     /* 每次 join 截断: 失败事件 grep 只看本次 */
        snprintf(cmd, sizeof cmd, "%s -B -Dnl80211 -i " STA_IF " -c %s -P %s -f %s",
                 WPA_SUP, WPA_CONF, WPA_PID, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_START, 0) != 0) return -1;
        g_j.sub = S_START;
        g_j.armed = 1;
        return 1;
    case S_START:
        if (g_ac.rc == 0) {
            g_j.last_poll = 0;
            j_phase(J_ASSOC);
            return 1;
        }
        LOGE("wifi_cam: wpa_supplicant 启动失败, tail %s:", WPA_LOG);
        snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);   /* 日志就地可见 */
        if (ac_spawn(cmd, 3000, 1) != 0) return -1;
        j_sub(S_TAIL);
        return 1;
    case S_TAIL:
        if (g_j.armed) {                       /* tail 输出已 capture, 打进服务日志 */
            char out[4096];
            ac_out(out, sizeof out);
            LOGW("wifi_cam: wpa.log 尾部:\n%.3800s", out);
        }
        return -1;
    default:
        return -1;
    }
}

static int step_assoc(void)
{
    long long now = now_ms();
    char cmd[512];                             /* §23: 组合轮询命令 ~190B, 160 会截断 */

    if (g_j.sub == A_TAIL) {                    /* 关联超时后的 tail 收尾 */
        if (ac_busy()) return 1;
        char out[4096];
        ac_out(out, sizeof out);
        LOGW("wifi_cam: wpa.log 尾部:\n%.3800s", out);
        return -1;
    }
    if (now - g_j.t_phase > T_ASSOC_MS) {
        LOGE("wifi_cam: 关联超时(%dms, 最后 wpa_state=%s)", T_ASSOC_MS,
             g_j.last_state[0] ? g_j.last_state : "?");
        snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);
        ac_spawn(cmd, 3000, 1);                /* 尽力而为(失败下轮直接退出) */
        g_j.sub = A_TAIL;
        g_j.armed = 1;
        return 1;
    }
    if (g_j.last_poll && now - g_j.last_poll < 1000)
        return 1;                              /* 1s 轮询节流 */
    if (!g_j.armed) {
        /* status + 失败事件 grep 一条命令(单 g_ac 槽位): reason=15(4次握手超时=密码错)
         * / reason=2(认证超时) —— 命中即快速失败, 不再傻等 45s 总超时(§23 用户点) */
        snprintf(cmd, sizeof cmd,
                 "%s -p %s status; echo __EV__; "
                 "grep -E \"reason=(15|2 |WRONG_KEY) |WRONG_KEY\" %s 2>/dev/null | tail -3",
                 WPA_CLI, WPA_CTRL, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_POLL, 1) != 0) return -1;
        g_j.armed = 1;
        return 1;
    }
    /* status 收尸: 解析 wpa_state */
    char out[2048], st[32] = "";
    ac_out(out, sizeof out);
    g_j.last_poll = now;
    g_j.armed = 0;
    line_get(out, "wpa_state", st, sizeof st);
    if (st[0] && strcmp(st, g_j.last_state) != 0) {
        LOGI("wifi_cam: wpa_state: %s → %s (%.1fs)",
             g_j.last_state[0] ? g_j.last_state : "(init)", st,
             (now - g_j.t_phase) / 1000.0);
        snprintf(g_j.last_state, sizeof g_j.last_state, "%s", st);
    }
    /* 失败事件区(__EV__ 之后): 4 次握手超时/认证超时 = 密码错误, 快速失败 */
    const char *ev = strstr(out, "__EV__");
    if (ev && (strstr(ev, "reason=15") || strstr(ev, "reason=2 ")
               || strstr(ev, "WRONG_KEY"))) {    /* WRONG_KEY: AP 明示密钥错(米家1S 实测) */
        LOGE("wifi_cam: 认证/握手失败(reason=15/2)——密码错误或 AP 拒绝, 快速失败 (%.1fs)",
             (now - g_j.t_phase) / 1000.0);
        g_join_fail_fast = 1;
        return -1;
    }
    if (strcmp(st, "COMPLETED") == 0) {
        char ssid_got[64] = "", bssid[32] = "";
        line_get(out, "ssid", ssid_got, sizeof ssid_got);
        line_get(out, "bssid", bssid, sizeof bssid);
        LOGI("wifi_cam: 4 次握手完成 ssid=%s bssid=%s (%.1fs)", ssid_got, bssid,
             (now - g_j.t_phase) / 1000.0);
        /* §23: 凭据只在 join 尝试期间落盘——握手完成即删(supplicant 已在内存,
         * 不再依赖配置文件); 磁盘暴露窗口从整个会话缩到 ~10s */
        remove(PSK_FILE);
        remove(WPA_CONF);
        LOGI("wifi_cam: psk/wpa.conf 已从磁盘删除(supplicant 内存持有)");
        j_phase(J_DHCP);
    }
    return 1;
}

static int step_dhcp(void)
{
    char cmd[192];
    switch (g_j.sub) {
    case D_RUN:
        if (!g_j.armed) {
            snprintf(cmd, sizeof cmd, "busybox udhcpc -i " STA_IF " -s %s -n -q -t 4 -T 3",
                     UDHCPC_SH);
            if (ac_spawn(cmd, T_DHCP, 0) != 0) return -1;
            g_j.armed = 1;
            return 1;
        }
        j_sub(g_ac.rc == 0 ? D_IFCFG : D_IFCFG_FAIL);
        return step_dhcp();
    case D_IFCFG:
    case D_IFCFG_FAIL:
        if (!g_j.armed) {
            if (ac_spawn("busybox ifconfig " STA_IF, 4000, 1) != 0) {
                /* 读不到也不影响结论 */
                if (g_j.sub == D_IFCFG_FAIL) return -1;
                j_phase(J_DONE);
                break;
            }
            g_j.armed = 1;
            return 1;
        }
        {
            char out[2048];
            ac_out(out, sizeof out);
            LOGI("wifi_cam: ifconfig: %.400s", out);
        }
        if (g_j.sub == D_IFCFG_FAIL) {
            LOGE("wifi_cam: udhcpc 未取得租约");
            return -1;
        }
        j_phase(J_DONE);
        break;
    default:
        return -1;
    }
    LOGI("wifi_cam: join 完成(insmod→assoc→dhcp), 总耗时 %.1fs",
         (now_ms() - g_j.t_join) / 1000.0);
    return 0;                                     /* 0=joined */
}

int wifi_cam_join_step(void)
{
    if (g_abort || g_j.phase == J_IDLE) return -1;
    if (g_j.phase == J_DONE) return 0;
    if (ac_busy() && ac_poll() == AC_RUNNING) return 1;   /* 当前步在跑(非阻塞) */

    switch (g_j.phase) {
    case J_PREP: {
        int r = chip_step();
        if (r < 0) return -1;
        if (r == 0) { j_phase(J_SUPP); return 1; }
        return 1;
    }
    case J_SUPP:  return step_supp();
    case J_ASSOC: return step_assoc();
    case J_DHCP:  return step_dhcp();
    default:      return -1;
    }
}

/* ---------------- §20 SSID 扫描选择(tick 步进) ---------------- */

/* 扫描候选(解析后中转; 去重/排序在小数组上就地进行) */
#define SCAN_MAX_NETS 32        /* 与 state.h PRN_MAX_SCAN 同值(INTERFACE.md §3: ≤32 项) */
typedef struct {
    char ssid[33];       /* 802.11 SSID ≤32 字节 + NUL */
    int  rssi;           /* dBm */
    int  wpa2;           /* 1=wpa2 0=open */
} scan_net_t;

/* 同 SSID 取最强; 保持 RSSI 降序插入 */
static void scan_push(scan_net_t *list, int *n, const char *ssid, int rssi, int wpa2)
{
    if (!ssid[0]) return;                                /* 隐藏网络跳过(§20) */
    for (int i = 0; i < *n; i++) {
        if (!strcmp(list[i].ssid, ssid)) {
            if (rssi > list[i].rssi) { list[i].rssi = rssi; list[i].wpa2 = wpa2; }
            return;                                      /* 去重 */
        }
    }
    if (*n >= SCAN_MAX_NETS) return;                     /* ≤32 项 */
    snprintf(list[*n].ssid, sizeof list[*n].ssid, "%s", ssid);
    list[*n].rssi = rssi;
    list[*n].wpa2 = wpa2;
    (*n)++;
}

static void scan_sort(scan_net_t *list, int n)
{
    for (int i = 1; i < n; i++) {                        /* 插入排序(RSSI 降序) */
        scan_net_t k = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < k.rssi) { list[j + 1] = list[j]; j--; }
        list[j + 1] = k;
    }
}

/* 去行首缩进/行尾换行, 便于 strncmp/sscanf; 返回行内容指针(常驻文本) */
static const char *trim_line(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* 解析 `iw dev wlan0 scan` 输出(BSS 块: SSID:/signal:/capability:/RSN:) */
static int parse_iw_scan(const char *out, scan_net_t *list, int *n)
{
    const char *p = out;
    char ssid[33] = "";
    int rssi = 0, wpa2 = 0, in_bss = 0, got = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        const char *t = trim_line(line);

        if (!strncmp(t, "BSS ", 4)) {
            if (in_bss) { scan_push(list, n, ssid, rssi, wpa2); got++; }
            in_bss = 1; ssid[0] = 0; rssi = -200; wpa2 = 0;
        } else if (in_bss && !strncmp(t, "SSID:", 5)) {
            snprintf(ssid, sizeof ssid, "%.32s", trim_line(t + 5));
        } else if (in_bss && !strncmp(t, "signal:", 7)) {
            rssi = (int)atof(t + 7);
        } else if (in_bss && !strncmp(t, "capability:", 11)) {
            if (strstr(t, "Privacy")) wpa2 = 1;           /* 加密位(配合 RSN 判定) */
        } else if (in_bss && (!strncmp(t, "RSN:", 4) || !strncmp(t, "WPA:", 4))) {
            wpa2 = 1;                                    /* 明确的 WPA/RSN IE */
        }
        p = nl ? nl + 1 : NULL;
    }
    if (in_bss) { scan_push(list, n, ssid, rssi, wpa2); got++; }
    return got;
}

/* 解析 `wpa_cli scan_results`(tab 分隔: bssid/freq/signal/flags/ssid) */
static int parse_wpa_scan(const char *out, scan_net_t *list, int *n)
{
    const char *p = out;
    int got = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        if (!strncmp(line, "bssid", 5) || line[0] == 0) continue;  /* 表头/空行 */

        char *f1 = strchr(line, '\t');                   /* bssid|freq */
        if (!f1) continue;
        char *f2 = strchr(f1 + 1, '\t');                 /* freq|signal */
        if (!f2) continue;
        char *f3 = strchr(f2 + 1, '\t');                 /* signal|flags */
        if (!f3) continue;
        char *f4 = strchr(f3 + 1, '\t');                 /* flags|ssid(可无=隐藏) */
        if (!f4) continue;
        *f4 = 0;
        const char *ssid = f4 + 1;
        int rssi = atoi(f2 + 1);
        int wpa2 = (strstr(f3 + 1, "WPA2") || strstr(f3 + 1, "RSN") ||
                    strstr(f3 + 1, "WPA-PSK") || strstr(f3 + 1, "WPA-EAP")) ? 1 : 0;
        scan_push(list, n, ssid, rssi, wpa2);
        got++;
    }
    return got;
}

/* 扫描机: 芯片保障(共享芯片子机) → iw 路径 → wpa_cli 回退路径 → 汇总 */
enum { SC_IDLE = 0, SC_CHIP, SC_IW, SC_WPA_KILL, SC_WPA_START, SC_WPA_SCAN,
       SC_WPA_POLL, SC_TAIL, SC_BUILD };
static struct {
    int        phase;
    int        armed;
    int        tries;                 /* wpa_cli scan_results 轮询次数 */
    long long  t_poll;
    scan_net_t list[SCAN_MAX_NETS];
    int        cnt, via_iw;
    int        fail;
} g_sc;

int wifi_cam_scan_begin(void)
{
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE) {
        LOGW("wifi_cam: join 进行中(%s), 拒绝扫描", wifi_cam_join_phase());
        return -1;
    }
    if (g_sc.phase != SC_IDLE) {
        LOGW("wifi_cam: 扫描进行中, 拒绝重复 start");
        return -1;
    }
    mkdirs();
    g_abort = 0;
    memset(g_sc.list, 0, sizeof g_sc.list);
    g_sc.cnt = 0;
    g_sc.via_iw = 0;
    g_sc.fail = 0;
    g_sc.armed = 0;
    chip_begin();
    g_sc.phase = SC_CHIP;
    LOGI("wifi_cam: scan 受理(芯片保障 → iw → wpa_cli 回退)");
    return 0;
}

static void scan_finish_list(char ***out, int *n)
{
    scan_sort(g_sc.list, g_sc.cnt);
    char **items = calloc((size_t)g_sc.cnt + 1, sizeof(char *));
    if (!items) { g_sc.phase = SC_IDLE; *out = NULL; *n = 0; g_sc.fail = 1; return; }
    for (int i = 0; i < g_sc.cnt; i++) {
        char el[64];
        snprintf(el, sizeof el, "%s\t%d\t%s", g_sc.list[i].ssid, g_sc.list[i].rssi,
                 g_sc.list[i].wpa2 ? "wpa2" : "open");
        items[i] = strdup(el);
    }
    *out = items;
    *n = g_sc.cnt;
    LOGI("wifi_cam: scan 完成(%s): %d 网络, 芯片保持可用(看门狗/restore 收尾)",
         g_sc.via_iw ? "iw" : "wpa_cli", g_sc.cnt);
    g_sc.phase = SC_IDLE;
}

int wifi_cam_scan_step(char ***out, int *n)
{
    char cmd[300];
    *out = NULL;
    *n = 0;
    if (g_abort || g_sc.phase == SC_IDLE) return -1;
    if (ac_busy() && ac_poll() == AC_RUNNING) return 1;

    switch (g_sc.phase) {
    case SC_CHIP: {
        int r = chip_step();
        if (r < 0) { g_sc.phase = SC_IDLE; return -1; }   /* 芯片起不来: 无可保留 */
        if (r == 0) {
            /* 路径 A: iw dev wlan0 scan(无 supplicant 依赖, 首选; 缺失/EBUSY 回退 B) */
            g_sc.phase = SC_IW;
            g_sc.armed = 0;
        }
        return 1;
    }
    case SC_IW:
        if (!g_sc.armed) {
            if (ac_spawn("iw dev " STA_IF " scan", T_SCAN, 1) != 0) {
                g_sc.phase = SC_IDLE;
                return -1;
            }
            g_sc.armed = 1;
            return 1;
        }
        {
            static char buf[32768];
            ac_out(buf, sizeof buf);
            g_sc.armed = 0;
            if (g_ac.rc == 0 && strstr(buf, "BSS ")) {
                parse_iw_scan(buf, g_sc.list, &g_sc.cnt);
                g_sc.via_iw = 1;
                scan_finish_list(out, n);
                return g_sc.fail ? -1 : 0;
            }
            LOGW("wifi_cam: iw 路径不可用(rc=%d), 回退 wpa_cli", g_ac.rc);
        }
        /* 路径 B: 自带 wpa_supplicant + wpa_cli scan/scan_results(§14 静态版保证在) */
        if (!exists(WPA_SUP) || !exists(WPA_CLI)) {
            LOGE("wifi_cam: 缺 %s / %s 且 iw 不可用(先 service.sh install)", WPA_SUP, WPA_CLI);
            g_sc.phase = SC_IDLE;
            return -1;
        }
        {
            FILE *f = fopen(WPA_SCAN_CONF, "w");         /* 空网络表扫描配置(0600) */
            if (!f) {
                LOGE("wifi_cam: 写 %s 失败: %s", WPA_SCAN_CONF, strerror(errno));
                g_sc.phase = SC_IDLE;
                return -1;
            }
            fprintf(f, "# x2print 扫描配置(wifi_cam 自动生成, 0600, 无网络表)\n"
                       "ctrl_interface=%s\nap_scan=1\n", WPA_CTRL);
            fclose(f);
            chmod(WPA_SCAN_CONF, 0600);
        }
        g_sc.phase = SC_WPA_KILL;
        g_sc.armed = 0;
        return 1;
    case SC_WPA_KILL:
        if (!g_sc.armed) {
            snprintf(cmd, sizeof cmd, "killall wpa_supplicant 2>/dev/null; rm -rf %s %s",
                     WPA_CTRL, WPA_PID);
            if (ac_spawn(cmd, T_WPA_KILL, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
            g_sc.armed = 1;
            return 1;
        }
        snprintf(cmd, sizeof cmd, "%s -B -Dnl80211 -i " STA_IF " -c %s -P %s -f %s",
                 WPA_SUP, WPA_SCAN_CONF, WPA_PID, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_START, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
        g_sc.phase = SC_WPA_START;
        g_sc.armed = 1;
        return 1;
    case SC_WPA_START:
        if (g_ac.rc != 0) {
            LOGE("wifi_cam: 扫描用 wpa_supplicant 启动失败, tail %s:", WPA_LOG);
            snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);
            ac_spawn(cmd, 3000, 1);
            g_sc.phase = SC_TAIL;
            g_sc.armed = 1;
            return 1;
        }
        snprintf(cmd, sizeof cmd, "%s -p %s scan", WPA_CLI, WPA_CTRL);
        if (ac_spawn(cmd, T_WPA_POLL, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
        g_sc.phase = SC_WPA_SCAN;
        g_sc.armed = 1;
        return 1;
    case SC_WPA_SCAN:
        if (g_ac.rc != 0)
            LOGW("wifi_cam: wpa_cli scan 触发非零返回(继续取结果)");
        g_sc.phase = SC_WPA_POLL;
        g_sc.tries = 0;
        g_sc.t_poll = 0;
        g_sc.armed = 0;
        return 1;
    case SC_WPA_POLL:
        /* 轮询 scan_results(每 500ms, ≤8s; 首轮常在 ~2-4s 出全) */
        if (g_sc.t_poll && now_ms() - g_sc.t_poll < 500) return 1;
        if (!g_sc.armed) {
            snprintf(cmd, sizeof cmd, "%s -p %s scan_results", WPA_CLI, WPA_CTRL);
            if (ac_spawn(cmd, T_WPA_POLL, 1) != 0) { g_sc.phase = SC_IDLE; return -1; }
            g_sc.armed = 1;
            return 1;
        }
        {
            static char buf[16384];
            ac_out(buf, sizeof buf);
            g_sc.armed = 0;
            g_sc.t_poll = now_ms();
            g_sc.tries++;
            parse_wpa_scan(buf, g_sc.list, &g_sc.cnt);
            if (g_sc.cnt > 0 || g_sc.tries >= 16) {
                /* supplicant 保持运行(用户即将连接; join 重起自己的, restore 收尾) */
                scan_finish_list(out, n);
                return g_sc.fail ? -1 : 0;
            }
        }
        return 1;
    case SC_TAIL:
        if (!g_sc.armed) { g_sc.phase = SC_IDLE; return -1; }
        {
            char out2[4096];
            ac_out(out2, sizeof out2);
            LOGW("wifi_cam: wpa.log 尾部:\n%.3800s", out2);
        }
        g_sc.phase = SC_IDLE;
        return -1;
    default:
        g_sc.phase = SC_IDLE;
        return -1;
    }
}

/* 会话凭据清理: ssid/psk 均为会话态(恢复/崩溃残留一并清; 原则: 会话态不跨会话) */
void wifi_cam_psk_purge(void)
{
    if (remove(PSK_FILE) == 0) LOGI("wifi_cam: psk 凭据已清除(会话结束)");
}

/* ---------------- 恢复机(反序, 异步步进, 幂等) ----------------
 * 步表尽力而为(rc 不 gate); rmmod 仅当所有权在手, 成功清凭据。
 * wakelock 不在此释放 —— backend_real 在合法电源同步完成后调 wake_release。 */
static const struct { const char *cmd; int to_ms; int need_if; } rs_ops[] = {
    { WPA_CLI " -p " WPA_CTRL " terminate 2>/dev/null", T_TERM,    0 },
    { "killall wpa_supplicant 2>/dev/null",             T_WPA_KILL, 0 },
    { "rm -rf " WPA_CTRL " " WPA_PID,                   2000,       0 },
    { "ip addr flush dev " STA_IF " 2>/dev/null",       T_FLUSH,    1 },
    { "busybox ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH,  1 },
    { "ifconfig " STA_IF " down",                       T_DOWN,     1 },
    { "busybox ifconfig " STA_IF " down",               T_DOWN,     1 },
};
#define RS_N ((int)(sizeof rs_ops / sizeof rs_ops[0]))
#define RS_IDLE (-1)
static struct { int op; int armed; int skip_net; } g_rs;

void wifi_cam_restore_begin(void)
{
    ac_kill();                       /* 在途 insmod/udhcpc/扫描子进程立即终结 */
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE)
        LOGW("wifi_cam: restore: join 阶段机就地终止(%s)", wifi_cam_join_phase());
    g_j.phase = J_IDLE;              /* join 阶段机终止(任何阶段) */
    g_sc.phase = SC_IDLE;            /* 扫描机终止 */
    g_chip.sub = C_OFF;
    g_abort = 0;
    g_rs.op = 0;
    g_rs.armed = 0;
    g_rs.skip_net = !iface_present();
    LOGI("wifi_cam: restore 序列启动(wlan0 %s)", g_rs.skip_net ? "不在(跳过地址步)" : "在");
}

int wifi_cam_restore_step(void)
{
    if (g_rs.op == RS_IDLE) return 0;               /* 未在恢复/已完成(幂等) */

    if (g_rs.armed && ac_busy() && ac_poll() == AC_RUNNING) return 1;
    if (g_rs.armed) {
        g_rs.armed = 0;
        g_rs.op++;                                  /* 普通步尽力而为, rc 不 gate */
    }

    while (g_rs.op < RS_N && rs_ops[g_rs.op].need_if && g_rs.skip_net)
        g_rs.op++;                                  /* wlan0 不在: 跳过地址步 */

    if (g_rs.op >= RS_N) {
        /* 末步: 模块驻留, 不 rmmod(§16.4 竞态根治: rmmod 后本开机内重载会撞
         * SDIO 枚举竞态 err=-19, 实锤 2026-09-26 verify 复盘; 模块驻留即原生
         * 状态——原生 boot 后模块也常驻, wms 的 power_on 走 devreset 不依赖
         * 重新 insmod。重启自然清零。) */
        remove(MOD_OWNED);                          /* 凭据作废(无人再据此 rmmod) */
        g_mod_owned = 0;
        g_rs.op = RS_IDLE;
        LOGI("wifi_cam: restore 完成(bcmdhd 驻留不卸, 合法关电即基线; §16.4)");
        return 0;
    }
    if (ac_spawn(rs_ops[g_rs.op].cmd, rs_ops[g_rs.op].to_ms, 0) != 0) {
        g_rs.op++;                                  /* spawn 失败(极少): 跳该步 */
        return wifi_cam_restore_step();
    }
    g_rs.armed = 1;
    return 1;
}

/* ---------------- 中断位(P0: 断开/取消即时生效) ---------------- */
void wifi_cam_abort(void)
{
    int active = (g_j.phase != J_IDLE && g_j.phase != J_DONE) ||
                 g_sc.phase != SC_IDLE || ac_busy();
    ac_kill();
    g_j.phase = J_IDLE;
    g_sc.phase = SC_IDLE;
    g_chip.sub = C_OFF;
    if (active) LOGW("wifi_cam: abort(join/扫描在途, 子进程已终结, 状态机复位)");
    g_abort = 1;                    /* 由 join_start/scan_begin/restore_begin 清零 */
}

/* ---------------- 启动残留清理(P1②: SIGKILL/掉电后收敛) ----------------
 * 同步一次性(boot 阶段, 总线客户端尚未排队); 全程幂等+日志:
 *   1. 陈旧 wpa_supplicant: pid 文件精确 kill(校验 cmdline 归属) + pgrep -f
 *      自带路径兜底(hostapd 等原生进程不中);
 *   2. module.owned 凭据 → rmmod(接口不在=已卸, 清凭据);
 *   3. wlan0 地址 flush+down: 仅当上述任一凭据在案(确属我们残留);
 *      凭据全无时绝不动 wlan0(可能是原生 AP 在用, 审计 P2-8)。 */
int wifi_cam_boot_cleanup(void)
{
    int stale_supp = 0, owned = exists(MOD_OWNED);

    /* 1. pid 文件精确 kill */
    FILE *f = fopen(WPA_PID, "r");
    if (f) {
        int pid = 0;
        int got = fscanf(f, "%d", &pid);
        fclose(f);
        remove(WPA_PID);
        if (got == 1 && pid > 1) {
            char fn[64], cl[160];
            snprintf(fn, sizeof fn, "/proc/%d/cmdline", pid);
            cl[0] = 0;
            FILE *cf = fopen(fn, "r");
            if (cf) {
                size_t n = fread(cl, 1, sizeof cl - 1, cf);
                cl[n] = 0;
                for (size_t i = 0; i < n; i++) if (cl[i] == 0) cl[i] = ' ';  /* NUL→空格 */
                fclose(cf);
            }
            if (strstr(cl, WPA_SUP)) {              /* 确认是我们的 supplicant */
                kill(pid, SIGTERM);
                for (int i = 0; i < 10 && kill(pid, 0) == 0; i++)
                    usleep(100 * 1000);             /* ≤1s 优雅 */
                if (kill(pid, 0) == 0) {
                    kill(pid, SIGKILL);
                    LOGW("wifi_cam boot: 陈旧 wpa_supplicant pid=%d SIGKILL(1s 未退)", pid);
                }
                stale_supp = 1;
                LOGI("wifi_cam boot: 清陈旧 wpa_supplicant pid=%d(pid 文件凭据)", pid);
            }
        }
    }
    /* 1b. pgrep 自带路径兜底(pid 文件丢失但进程残留; -f 全路径匹配, 原生不中) */
    {
        char out[256];
        if (run_cmd_out("pgrep -f " WPA_SUP, 3000, out, sizeof out) == 0 && out[0]) {
            int killed = 0;
            for (char *p = out; p && *p; ) {
                int pid = (int)strtol(p, &p, 10);
                if (pid > 1) { kill(pid, SIGKILL); killed++; }
                while (*p == ' ' || *p == '\n') p++;
            }
            if (killed) {
                stale_supp = 1;
                LOGI("wifi_cam boot: pgrep 兜底清 %d 个残留 supplicant(自带路径)", killed);
            }
        }
    }
    run_cmd("rm -rf " WPA_CTRL " " WPA_PID, 2000);
    wifi_cam_psk_purge();                            /* 崩溃残留 psk 一并清 */

    /* 2. 模块不卸(§16.4 根治): 残留 bcmdhd 驻留无害(=原生状态, wms power_on 走
     *    devreset 不依赖重载; 重启自清)。凭据只清理, 不据以 rmmod。 */
    if (owned) {
        if (iface_present())
            run_cmd("ifconfig " STA_IF " down 2>/dev/null", T_DOWN);
        remove(MOD_OWNED);
        LOGI("wifi_cam boot: module.owned 凭据清理(模块驻留不卸, §16.4)");
    }

    /* 3. wlan0 地址 flush+down: 仅凭据在案(我们的残留)时 */
    if (stale_supp || owned) {
        run_cmd("ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH);
        run_cmd("busybox ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH);
        run_cmd("ifconfig " STA_IF " down", T_DOWN);
        LOGI("wifi_cam boot: wlan0 地址 flush+down(残留凭据在案)");
    } else {
        LOGI("wifi_cam boot: 无本产品 Wi-Fi 残留凭据, wlan0 不动(可能是原生 AP)");
    }
    return 0;
}

#else /* !__ANDROID__ —— WSL/PC 弱化桩: 可编译可链接 */

static const char *g_offcam =
    "wifi_cam: 非相机环境(__ANDROID__ 未定义), 编排为桩(联调请 wifi_backend=mock)";

int wifi_cam_join_start(const char *s, const char *p)
{
    (void)s; (void)p; LOGW("%s", g_offcam); return -1;
}
int wifi_cam_join_step(void)              { return -1; }
const char *wifi_cam_join_phase(void)     { return "stub"; }
int wifi_cam_scan_begin(void)             { LOGW("%s", g_offcam); return -1; }
int wifi_cam_scan_step(char ***out, int *n) { (void)out; (void)n; return -1; }
void wifi_cam_restore_begin(void)         { }
int wifi_cam_restore_step(void)           { return 0; }
void wifi_cam_abort(void)                 { }
int wifi_cam_boot_cleanup(void)           { return 0; }
void wifi_cam_wake_hold(void)             { }
void wifi_cam_wake_release(void)          { }
void wifi_cam_psk_purge(void)               { }   /* 桩: 无 psk 文件 */

#endif /* __ANDROID__ */
