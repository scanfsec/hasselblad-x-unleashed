/* state.c — 属性存储 + 校验 + 变更通知(见 state.h / INTERFACE.md §2 v2)
 * v2: media/print_quality/color_mode 字符串化, 能力属性由 prn_caps_publish
 * 填充; Set 校验以能力列表为源(caps_ready 后)。 */
#include "state.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static prn_state g_st;             /* 零初始化, init() 填默认值 */
static prn_notify_fn g_notify;
static void *g_notify_user;

static const prn_strlist_t g_empty_list = { { { 0 } }, 0 };

/* 属性元信息表: 顺序=枚举=GetAll 输出顺序, 与 INTERFACE.md §2 逐行对齐 */
static const prn_prop_info g_props[PRN_PROP_COUNT] = {
    [PRN_CONNECTION_STATE]       = { "connection_state",       's', 0, "disconnected|joining|joined|discovering|ready" },
    [PRN_SSID]                   = { "ssid",                   's', 1, "target AP ssid" },
    [PRN_PRINTER_STATE]          = { "printer_state",          's', 0, "idle|busy|error" },
    [PRN_PRINTER_NAME]           = { "printer_name",           's', 0, "selected printer" },
    [PRN_PRINTER_IP]             = { "printer_ip",             's', 0, "selected printer ip" },
    [PRN_JOB_PROGRESS]           = { "job_progress",           'i', 0, "0..100" },
    [PRN_JOB_STATUS]             = { "job_status",             's', 0, "idle|preparing|sending|printing|done|failed|canceled" },
    [PRN_COPIES]                 = { "copies",                 'i', 1, "1..copies_max(caps 未就绪=99)" },
    [PRN_MEDIA]                  = { "media",                  's', 1, "打印机原生 media 关键字(取自 supported_media)" },
    [PRN_PRINT_QUALITY]          = { "print_quality",          's', 1, "draft|normal|high(取自 supported_qualities)" },
    [PRN_COLOR_MODE]             = { "color_mode",             's', 1, "原生 print-color-mode 关键字(取自 supported_color_modes)" },
    [PRN_PRESET]                 = { "preset",                 'i', 1, "0 default 1 id 2 polaroid(应用层)" },
    [PRN_DOCUMENT_FORMAT]        = { "document_format",        's', 0, "实际下发 document-format(只读)" },
    [PRN_CAPS_READY]             = { "caps_ready",             'b', 0, "能力探测完成" },
    [PRN_COPIES_MAX]             = { "copies_max",             'i', 0, "copies-supported 上限" },
    [PRN_SUPPORTED_MEDIA]        = { "supported_media",        'A', 0, "as: 原生 media 关键字" },
    [PRN_SUPPORTED_QUALITIES]    = { "supported_qualities",    'A', 0, "as: draft/normal/high 子集" },
    [PRN_SUPPORTED_COLOR_MODES]  = { "supported_color_modes",  'A', 0, "as: 原生 color-mode 关键字" },
    [PRN_SUPPORTED_FORMATS]      = { "supported_formats",      'A', 0, "as: document-format-supported" },
    [PRN_MEDIA_SIZE_INFO]        = { "media_size_info",        'A', 0, "as: 每项 keyword=WxHmm" },
    [PRN_DISCOVERED_PRINTERS]    = { "discovered_printers",    'A', 0, "array of \"name\\tIP\"" },
    [PRN_SCANNED_NETWORKS]       = { "scanned_networks",       'A', 0, "as: \"SSID\\tRSSI(dBm)\\tSEC\"(wpa2|open), §20 Scan_wifi" },
    /* odindb-send-printer 兼容面(INTERFACE.md §7b): 补丁槽位固定属性, 只读 */
    [PRN_SYNCED]                 = { "synced",                 'b', 0, "odindb usbhost-slot compat" },
    [PRN_TX_ACTIVE]              = { "tx_active",              'b', 0, "true while tx() runs" },
};

const prn_prop_info *prn_prop_info_get(int idx)
{
    return (idx >= 0 && idx < PRN_PROP_COUNT) ? &g_props[idx] : NULL;
}

int prn_prop_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < PRN_PROP_COUNT; i++)
        if (!strcmp(g_props[i].name, name)) return i;
    return -1;
}

void prn_state_set_notify(prn_notify_fn fn, void *user)
{
    g_notify = fn;
    g_notify_user = user;
}

void prn_prop_changed(int idx)
{
    if (g_notify) g_notify(idx, g_notify_user);
}

prn_state *prn_state_get(void)
{
    static int inited = 0;
    if (!inited) {
        inited = 1;
        /* 默认值 = INTERFACE.md §2 v2 初始值列 */
        strcpy(g_st.connection_state, "disconnected");
        g_st.ssid[0] = 0;
        strcpy(g_st.printer_state, "idle");
        g_st.printer_name[0] = 0;
        g_st.printer_ip[0] = 0;
        g_st.job_progress = 0;
        strcpy(g_st.job_status, "idle");
        g_st.copies = 1;
        g_st.media[0] = 0;                    /* 探测后=打印机 media-default */
        strcpy(g_st.print_quality, "normal"); /* 探测后=打印机 quality-default */
        strcpy(g_st.color_mode, "auto");      /* 探测后=打印机 color-default */
        g_st.preset = 0;
        g_st.document_format[0] = 0;
        g_st.caps_ready = 0;
        g_st.copies_max = 99;                 /* 未探测前的宽容上限 */
        g_st.discovered_n = 0;
        g_st.scanned.n = 0;            /* §20: 扫描列表初始空 */
        g_st.synced = 1;               /* §7b 初始值: synced=true */
        g_st.tx_active = 0;            /* §7b 初始值: tx_active=false */
    }
    return &g_st;
}

/* ---- 可写属性校验 ---- */
static int int_range_ok(int idx, int v)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_COPIES:          return v >= 1 && v <= st->copies_max;
    case PRN_PRESET:          return v >= 0 && v <= 2;
    default:                  return 0;
    }
}

/* 字符串可写属性语义校验: caps_ready 后必须命中能力列表 */
static int str_value_ok(int idx, const char *v)
{
    prn_state *st = prn_state_get();
    if (!st->caps_ready) return 1;             /* 未探测: 暂无能力可校验 */
    switch (idx) {
    case PRN_MEDIA:         return prn_list_contains(&st->sup_media, v);
    case PRN_PRINT_QUALITY: return prn_list_contains(&st->sup_qualities, v);
    case PRN_COLOR_MODE:    return prn_list_contains(&st->sup_color_modes, v);
    default:                return 1;
    }
}

int prn_set_int(int idx, int val)
{
    const prn_prop_info *pi = prn_prop_info_get(idx);
    if (!pi) { LOGW("set: unknown property #%d", idx); return -1; }
    if (!pi->writable || pi->type != 'i') {
        LOGW("set: property '%s' not writable int", pi->name);
        return -1;
    }
    if (!int_range_ok(idx, val)) {
        LOGW("set: property '%s' value %d out of range", pi->name, val);
        return -2;
    }
    int *dst = NULL;
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_COPIES: dst = &st->copies; break;
    case PRN_PRESET: dst = &st->preset; break;
    default: return -1;
    }
    if (*dst != val) { *dst = val; prn_prop_changed(idx); }
    return 0;
}

int prn_set_string(int idx, const char *val)
{
    const prn_prop_info *pi = prn_prop_info_get(idx);
    if (!pi || !val) { LOGW("set: bad string set #%d", idx); return -1; }
    if (!pi->writable || pi->type != 's') {
        LOGW("set: property '%s' not writable string", pi->name);
        return -1;
    }
    if (!*val && idx != PRN_SSID && idx != PRN_MEDIA) {
        LOGW("set: property '%s' empty value", pi->name); /* media 允许清空(未选打印机) */
        return -2;
    }
    if (strlen(val) >= PRN_SLEN) {
        LOGW("set: property '%s' value too long", pi->name);
        return -2;
    }
    if (!str_value_ok(idx, val)) {
        LOGW("set: property '%s' value '%s' not in supported list", pi->name, val);
        return -2;
    }
    prn_state *st = prn_state_get();
    char *dst = NULL;
    switch (idx) {
    case PRN_SSID:          dst = st->ssid;          break;
    case PRN_MEDIA:         dst = st->media;         break;
    case PRN_PRINT_QUALITY: dst = st->print_quality; break;
    case PRN_COLOR_MODE:    dst = st->color_mode;    break;
    default: return -1;
    }
    if (strcmp(dst, val)) {
        strcpy(dst, val);
        prn_prop_changed(idx);
    }
    return 0;
}

/* 字符串取值: 返回静态数组(永不 NULL, 封送层可安全直接用); 未知 idx 返回 "" */
const char *prn_get_string(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_CONNECTION_STATE: return st->connection_state;
    case PRN_SSID:             return st->ssid;
    case PRN_PRINTER_STATE:    return st->printer_state;
    case PRN_PRINTER_NAME:     return st->printer_name;
    case PRN_PRINTER_IP:       return st->printer_ip;
    case PRN_JOB_STATUS:       return st->job_status;
    case PRN_MEDIA:            return st->media;
    case PRN_PRINT_QUALITY:    return st->print_quality;
    case PRN_COLOR_MODE:       return st->color_mode;
    case PRN_DOCUMENT_FORMAT:  return st->document_format;
    default:                   return "";
    }
}int prn_get_int(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_JOB_PROGRESS:    return st->job_progress;
    case PRN_COPIES:          return st->copies;
    case PRN_PRESET:          return st->preset;
    case PRN_CAPS_READY:      return st->caps_ready;
    case PRN_COPIES_MAX:      return st->copies_max;
    case PRN_SYNCED:          return st->synced;
    case PRN_TX_ACTIVE:       return st->tx_active;
    default:                  return 0;
    }
}

const prn_strlist_t *prn_get_list(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_SUPPORTED_MEDIA:       return &st->sup_media;
    case PRN_SUPPORTED_QUALITIES:   return &st->sup_qualities;
    case PRN_SUPPORTED_COLOR_MODES: return &st->sup_color_modes;
    case PRN_SUPPORTED_FORMATS:     return &st->sup_formats;
    case PRN_MEDIA_SIZE_INFO:       return &st->media_size_info;
    case PRN_SCANNED_NETWORKS:      return &st->scanned;
    default:                        return &g_empty_list;
    }
}

int prn_list_contains(const prn_strlist_t *l, const char *v)
{
    if (!l || !v) return 0;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->items[i], v)) return 1;
    return 0;
}

/* ---- 能力发布 / 失效 ---- */

/* 列表填入(静默覆盖; notify 由调用方统一做) */
static void list_fill(prn_strlist_t *l, const char *const *vals, int n)
{
    l->n = 0;
    for (int i = 0; i < n && i < PRN_MAX_LIST; i++) {
        if (!vals[i] || !vals[i][0]) continue;
        snprintf(l->items[l->n], PRN_LIST_LEN, "%.*s", PRN_LIST_LEN - 1, vals[i]);
        l->n++;
    }
}

static void caps_notify_lists(void)
{
    prn_prop_changed(PRN_SUPPORTED_MEDIA);
    prn_prop_changed(PRN_SUPPORTED_QUALITIES);
    prn_prop_changed(PRN_SUPPORTED_COLOR_MODES);
    prn_prop_changed(PRN_SUPPORTED_FORMATS);
    prn_prop_changed(PRN_MEDIA_SIZE_INFO);
    prn_prop_changed(PRN_COPIES_MAX);
}

void prn_caps_publish(const ipp_caps_t *caps, const char *document_format)
{
    prn_state *st = prn_state_get();
    if (!caps) return;

    /* 1) 能力列表 */
    const char *medias[IPP_MAX_MEDIA];
    int nm = 0;
    char sizes[IPP_MAX_MEDIA][PRN_LIST_LEN];
    const char *size_ptrs[IPP_MAX_MEDIA];
    int ns = 0;
    for (int i = 0; i < caps->n_media && nm < IPP_MAX_MEDIA; i++) {
        if (!caps->media_native[i]) continue;      /* 合成显示名不可下发, 不进 supported */
        medias[nm++] = caps->media_supported[i];
        if (caps->media_x_um[i] > 0 && caps->media_y_um[i] > 0 && ns < IPP_MAX_MEDIA) {
            /* media_size_info: "keyword=WxHmm"(W/H 毫米, 人机两读) */
            snprintf(sizes[ns], PRN_LIST_LEN, "%s=%.1fx%.1fmm", caps->media_supported[i],
                     caps->media_x_um[i] / 1000.0, caps->media_y_um[i] / 1000.0);
            size_ptrs[ns] = sizes[ns];
            ns++;
        }
    }
    list_fill(&st->sup_media, medias, nm);
    list_fill(&st->media_size_info, size_ptrs, ns);

    const char *quals[IPP_MAX_QUALS];
    int nq = 0;
    for (int i = 0; i < caps->n_quals && nq < IPP_MAX_QUALS; i++) {
        const char *kw = ipp_quality_kw(caps->qualities[i]);
        if (kw[0]) quals[nq++] = kw;               /* 仅认识 3/4/5, 其它枚举值跳过 */
    }
    list_fill(&st->sup_qualities, quals, nq);

    const char *modes[IPP_MAX_MODES];
    for (int i = 0; i < caps->n_modes && i < IPP_MAX_MODES; i++)
        modes[i] = caps->print_color_modes[i];
    list_fill(&st->sup_color_modes, modes, caps->n_modes);

    const char *fmts[IPP_MAX_FORMATS];
    for (int i = 0; i < caps->n_formats && i < IPP_MAX_FORMATS; i++)
        fmts[i] = caps->document_formats[i];
    list_fill(&st->sup_formats, fmts, caps->n_formats);

    st->copies_max = caps->copies_max > 0 ? caps->copies_max : 99;
    caps_notify_lists();

    /* 2) 打印机默认值归一(先置 ready 再校验默认值合法性) */
    st->caps_ready = 1;
    prn_prop_changed(PRN_CAPS_READY);

    if (caps->media_default[0] && prn_list_contains(&st->sup_media, caps->media_default))
        prn_set_string(PRN_MEDIA, caps->media_default);
    else if (st->sup_media.n > 0)
        prn_set_string(PRN_MEDIA, st->sup_media.items[0]);
    else
        prn_set_string(PRN_MEDIA, "");

    const char *qdef = ipp_quality_kw(caps->quality_default);
    if (!qdef[0]) qdef = "normal";
    if (st->sup_qualities.n == 0 || prn_list_contains(&st->sup_qualities, qdef))
        prn_set_string(PRN_PRINT_QUALITY, qdef);
    else
        prn_set_string(PRN_PRINT_QUALITY, st->sup_qualities.items[0]);

    if (caps->color_mode_default[0] &&
        prn_list_contains(&st->sup_color_modes, caps->color_mode_default))
        prn_set_string(PRN_COLOR_MODE, caps->color_mode_default);
    else if (st->sup_color_modes.n > 0)
        prn_set_string(PRN_COLOR_MODE, st->sup_color_modes.items[0]);

    if (document_format && document_format[0]) {
        if (strcmp(st->document_format, document_format)) {
            snprintf(st->document_format, sizeof st->document_format, "%s", document_format);
            prn_prop_changed(PRN_DOCUMENT_FORMAT);
        }
    }

    /* 3) copies 超限钳制(越限值不再合法) */
    if (st->copies > st->copies_max) {
        st->copies = st->copies_max;
        prn_prop_changed(PRN_COPIES);
    }
    LOGI("caps: published media=%d size_info=%d qualities=%d modes=%d formats=%d "
         "copies_max=%d res=%ddpi docfmt=%s",
         st->sup_media.n, st->media_size_info.n, st->sup_qualities.n,
         st->sup_color_modes.n, st->sup_formats.n, st->copies_max,
         caps->res_default_x, st->document_format);
}

void prn_caps_invalidate(void)
{
    prn_state *st = prn_state_get();
    if (!st->caps_ready && st->sup_media.n == 0 && st->copies_max == 99) return;
    st->caps_ready = 0;
    st->copies_max = 99;
    st->sup_media.n = 0;
    st->sup_qualities.n = 0;
    st->sup_color_modes.n = 0;
    st->sup_formats.n = 0;
    st->media_size_info.n = 0;
    if (st->document_format[0]) { st->document_format[0] = 0; prn_prop_changed(PRN_DOCUMENT_FORMAT); }
    if (st->copies > st->copies_max) { st->copies = st->copies_max; prn_prop_changed(PRN_COPIES); }
    prn_prop_changed(PRN_CAPS_READY);
    prn_prop_changed(PRN_COPIES_MAX);
    caps_notify_lists();
    LOGI("caps: invalidated");
}

/* ---- discovered_printers ---- */
void prn_discovered_clear(void)
{
    prn_state *st = prn_state_get();
    int had = st->discovered_n > 0;
    for (int i = 0; i < st->discovered_n; i++) free(st->discovered[i]);
    st->discovered_n = 0;
    if (had) prn_prop_changed(PRN_DISCOVERED_PRINTERS);
}

int prn_discovered_add(const char *name_ip)
{
    prn_state *st = prn_state_get();
    if (st->discovered_n >= PRN_MAX_PRINTERS) { LOGW("discovered list full"); return -1; }
    char *dup = strdup(name_ip);
    if (!dup) { LOGE("oom discovered_add"); return -1; }
    st->discovered[st->discovered_n++] = dup;
    prn_prop_changed(PRN_DISCOVERED_PRINTERS);
    return 0;
}

/* ---- scanned_networks(§20 扫描选择) ---- */
void prn_scanned_clear(void)
{
    prn_state *st = prn_state_get();
    int had = st->scanned.n > 0;
    st->scanned.n = 0;
    if (had) prn_prop_changed(PRN_SCANNED_NETWORKS);
}

int prn_scanned_add(const char *ssid_rssi_sec)
{
    prn_state *st = prn_state_get();
    if (!ssid_rssi_sec || !ssid_rssi_sec[0]) return -1;
    if (st->scanned.n >= PRN_MAX_SCAN || st->scanned.n >= PRN_MAX_LIST) {
        LOGW("scanned list full(%d)", st->scanned.n);
        return -1;
    }
    if (strlen(ssid_rssi_sec) >= PRN_LIST_LEN) {
        LOGW("scanned item too long(%zu): %.32s...", strlen(ssid_rssi_sec), ssid_rssi_sec);
        return -1;
    }
    snprintf(st->scanned.items[st->scanned.n], PRN_LIST_LEN, "%s", ssid_rssi_sec);
    st->scanned.n++;
    prn_prop_changed(PRN_SCANNED_NETWORKS);
    return 0;
}

/* ---- 状态机便捷跳转(内部使用, 值不校验) ---- */
static void set_s(char *dst, const char *v, int idx)
{
    if (strcmp(dst, v)) {
        strncpy(dst, v, PRN_SLEN - 1);
        dst[PRN_SLEN - 1] = 0;
        prn_prop_changed(idx);
    }
}
void prn_set_connection_state(const char *v) { set_s(prn_state_get()->connection_state, v, PRN_CONNECTION_STATE); }
void prn_set_printer_state(const char *v)    { set_s(prn_state_get()->printer_state,    v, PRN_PRINTER_STATE); }
void prn_set_job_status(const char *v)       { set_s(prn_state_get()->job_status,       v, PRN_JOB_STATUS); }
void prn_set_printer_name(const char *v)     { set_s(prn_state_get()->printer_name,     v, PRN_PRINTER_NAME); }
void prn_set_printer_ip(const char *v)       { set_s(prn_state_get()->printer_ip,       v, PRN_PRINTER_IP); }

void prn_set_job_progress(int v)
{
    prn_state *st = prn_state_get();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (st->job_progress != v) { st->job_progress = v; prn_prop_changed(PRN_JOB_PROGRESS); }
}

/* odindb 兼容属性(§7b)翻转: 值规整 0/1, 变化才 notify */
void prn_set_synced(int v)
{
    prn_state *st = prn_state_get();
    v = v ? 1 : 0;
    if (st->synced != v) { st->synced = v; prn_prop_changed(PRN_SYNCED); }
}

void prn_set_tx_active(int v)
{
    prn_state *st = prn_state_get();
    v = v ? 1 : 0;
    if (st->tx_active != v) { st->tx_active = v; prn_prop_changed(PRN_TX_ACTIVE); }
}

int prn_job_active(void)
{
    return prn_job_status_is("preparing") || prn_job_status_is("sending") || prn_job_status_is("printing");
}
int prn_state_is(const char *v)      { return !strcmp(prn_state_get()->connection_state, v); }
int prn_job_status_is(const char *v) { return !strcmp(prn_state_get()->job_status, v); }
