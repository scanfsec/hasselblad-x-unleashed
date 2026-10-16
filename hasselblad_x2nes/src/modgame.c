/* modgame v3.1 - inline hook of QQmlComponent(QQmlEngine*, const QUrl&, ...)
 *
 * Qt is statically linked into camera-gui (no PLT to preload-over), so we
 * patch the ctor entry at base+0xc597d8 with an absolute jump to a gate
 * that (a) rewrites the QUrl argument when it is mainmenu/MainScreen.qml
 * to file:///data/x2nes/MainScreen.qml (a patched single-file copy; every
 * other QML keeps loading natively from qrc/AOT), and (b) re-executes the
 * displaced register-only prologue and jumps back into the original.
 * Keeps the fopen(/proc/cmdline) fake so hblProductInfo doesn't abort. */
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;

#define SYS_openat 56
#define SYS_read 63
#define SYS_write 64
#define SYS_close 57
#define SYS_mprotect 226

#define AT_FDCWD (-100)
#define O_RDONLY_ 0
#define O_WRONLY_ 1
#define O_RDWR_ 2
#define O_CREAT_ 0x40
#define O_TRUNC_ 0x200

#define QQMLCOMP_CTOR_OFF 0xc597d8UL
#define QURL_TOSTRING_OFF 0xe9c660UL

static s64 sys5(u64 n, u64 a, u64 b, u64 c, u64 d)
{
    register u64 x8 __asm__("x8") = n;
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    register u64 x3 __asm__("x3") = d;
    __asm__ volatile("svc 0" : "=r"(x0) : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (s64)x0;
}
static s64 sys3(u64 n, u64 a, u64 b, u64 c) { return sys5(n, a, b, c, 0); }

static int g_kmsg = -1;
static void klogs(const char *s)
{
    if (g_kmsg < 0)
        g_kmsg = (int)sys5(SYS_openat, (u64)AT_FDCWD, (u64)"/dev/kmsg", O_WRONLY_, 0);
    u64 n = 0;
    while (s[n]) n++;
    if (g_kmsg >= 0) sys3(SYS_write, (u64)g_kmsg, (u64)s, n);
}

static u64 g_base;
static u64 g_qt;

void *fopen(const char *path, const char *mode);
extern int getc_(void *) __asm__("getc");
extern int fclose_(void *) __asm__("fclose");
#define raw_getc getc_
extern void *fmemopen_(const char *, unsigned long, const char *)
    __asm__("fmemopen");
extern void *fdopen_(int, const char *) __asm__("fdopen");
extern int *__errno(void);

static u64 find_base_maps(void)
{
    /* pure syscalls: at lazy_init time (first fopen) libc stdio may not be
     * safe to re-enter from our own fopen hook */
    int mfd = (int)sys5(SYS_openat, (u64)AT_FDCWD, (u64)"/proc/self/maps", O_RDONLY_, 0);
    if (mfd < 0) return 0;
    static char buf[32768];
    int n = 0;
    for (;;) {
        s64 r = sys3(SYS_read, (u64)mfd, (u64)buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (int)r;
        if (n >= (int)sizeof buf - 1) break;
    }
    sys3(SYS_close, (u64)mfd, 0, 0);
    if (n > 60) {
        static char hx[64];
        static const char hexd[] = "0123456789abcdef";
        int o = 0;
        for (int i = 0; i < 16 && o < 60; i++) {
            unsigned char b = (unsigned char)buf[i];
            hx[o++] = hexd[b >> 4];
            hx[o++] = hexd[b & 15];
            hx[o++] = ' ';
        }
        hx[o++] = 10; hx[o] = 0;
        klogs("MODGAME: buf16 ");
        klogs(hx);
    }
    for (int ls = 0; ls < n; ) {
        int le = ls;
        while (le < n && buf[le] != 10) le++;
        if (le > ls + 20) {
            int t1s = ls, t1e = t1s; while (t1e < le && buf[t1e] != 32) t1e++;
            int t2s = t1e; while (t2s < le && buf[t2s] == 32) t2s++;
            int t2e = t2s; while (t2e < le && buf[t2e] != 32) t2e++;
            int t3s = t2e; while (t3s < le && buf[t3s] == 32) t3s++;
            int t3e = t3s; while (t3e < le && buf[t3e] != 32) t3e++;
            int isexec = (t2e >= t2s + 3 && buf[t2s + 2] == 120);
            int offzero = (t3e == t3s + 8);
            if (offzero)
                for (int q = t3s; q < t3e; q++)
                    if (buf[q] != 48) { offzero = 0; break; }
            if (isexec && offzero) {
                u64 start = 0;
                for (int q = t1s; q < t1e; q++) {
                    char ch = buf[q];
                    if (ch == 45) continue;   /* '-' inside START-END */
                    u64 d = (ch >= 48 && ch <= 57) ? (u64)(ch - 48)
                          : (ch >= 97 && ch <= 102) ? (u64)(ch - 87) : 16;
                    if (d == 16) { start = 0; break; }
                    start = (start << 4) | d;
                }
                if (start) return start;
            }
        }
        ls = le + 1;
    }
    return 0;
}

static long qurl_tostring(u64 qurl, u64 *out_ptr, u64 *out_size)
{
    register u64 x0 __asm__("x0") = qurl;
    register u64 x1 __asm__("x1") = 0;
    u64 fn = g_qt + QURL_TOSTRING_OFF;
    __asm__ volatile(
        "blr %[fn]\n"
        : "+r"(x0), "+r"(x1)
        : [fn] "r" (fn)
        : "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11",
          "x12","x13","x14","x15","x16","x17","memory","cc");
    *out_ptr = x0;
    *out_size = x1;
    return 0;
}

static const unsigned short NEWURL[] = {
    'f','i','l','e',':','/','/','/','d','a','t','a','/','x','2','n',
    'e','s','/','M','a','i','n','S','c','r','e','e','n','.','q','m','l', 0
};
#define NEWURL_LEN 33

static int patch_qurl(u64 qurl)
{
    u64 d = *(u64 *)qurl;
    if (!d) return 0;
    for (u64 off = 0; off < 0x120; off += 8) {
        u64 ptr = *(u64 *)(d + off);
        u64 sz  = *(u64 *)(d + off + 8);
        if (sz > 20 && sz < 200 && ptr > 0x1000) {
            unsigned short *w = (unsigned short *)ptr;
            if ((w[0] == 'q' && w[1] == 'r' && w[2] == 'c' && w[3] == ':') ||
                (w[0] == 'f' && w[1] == 'i' && w[2] == 'l' && w[3] == 'e')) {
                if (w[7] == 'M' && w[8] == 'a') {
                    for (u64 i = 0; i < NEWURL_LEN; i++) w[i] = NEWURL[i];
                    *(u64 *)(d + off + 8) = NEWURL_LEN;
                    return 1;
                }
            }
        }
    }
    return 0;
}

void hook_decide(u64 qurl)
{
    if (!g_qt) return;
    u64 ptr, size;
    qurl_tostring(qurl, &ptr, &size);
    if (size < 10 || size > 300 || ptr < 0x1000) return;
    unsigned short *w = (unsigned short *)ptr;
    for (u64 i = 0; i + 14 <= size; i++) {
        if (w[i] == 'M' && w[i+1] == 'a' && w[i+2] == 'i' && w[i+3] == 'n' &&
            w[i+10] == '.' && w[i+11] == 'q') {
            if (patch_qurl(qurl))
                klogs("MODGAME: MainScreen swapped\n");
            return;
        }
    }
}

extern void hook_trampoline(void);
extern void hook_gate(void);

__asm__(
".text\n"
".globl hook_trampoline\n"
"hook_trampoline:\n"
"    ldr x17, 1f\n"
"    br x17\n"
".balign 8\n"
"1:  .xword 0\n"
".globl hook_gate\n"
"hook_gate:\n"
"    stp x29, x30, [sp, #-128]!\n"
"    stp x0, x1, [sp, #16]\n"
"    stp x2, x3, [sp, #32]\n"
"    stp x4, x5, [sp, #48]\n"
"    mov x0, x2\n"
"    bl hook_decide\n"
"    ldp x0, x1, [sp, #16]\n"
"    ldp x2, x3, [sp, #32]\n"
"    ldp x4, x5, [sp, #48]\n"
"    ldp x29, x30, [sp], #128\n"
"    stp x29, x30, [sp, #-48]!\n"
"    str x21, [sp, #16]\n"
"    mov x29, sp\n"
"    stp x20, x19, [sp, #32]\n"
"    ldr x17, 2f\n"
"    br x17\n"
".balign 8\n"
"2:  .xword 0\n"
);

static volatile int g_hook_installed;

static void install_hook(void)
{
    if (g_hook_installed || !g_qt) return;
    u64 fn = g_qt + QQMLCOMP_CTOR_OFF;
    u64 page = fn & ~0xffful;
    u32 *code = (u32 *)fn;
    u64 gate = (u64)&hook_gate;
    u64 glit = 0;
    for (u64 o = 0x8; o < 0x90; o += 8) {
        if (*(u64 *)((u64)&hook_gate + o) == 0) { glit = (u64)&hook_gate + o; break; }
    }
    if (!glit) { klogs("MODGAME: gate lit missing\n"); return; }
    /* literal slots live in our own R-X .text: open them for writing */
    u64 spage = (u64)&hook_gate & ~0xffful;
    sys3(SYS_mprotect, spage, 0x2000, 7);
    *(u64 *)((u64)&hook_trampoline + 8) = gate;
    *(u64 *)glit = fn + 20;
    sys3(SYS_mprotect, spage, 0x2000, 5);
    /* strict-kernel-RWX kernel: in-process writes to camera-gui .text are
     * refused (mprotect ENOMEM, /proc/self/mem fail). Instead we publish
     * the patch payload to /tmp/modgame.hookinfo and an external root
     * process (rsh) pokes the 16 bytes via /proc/PID/mem right after boot. */
    {
        /* write "PID BASE GATE" info file */
        char line[96];
        int o = 0;
        const char *tag = "gate=";
        for (int i = 0; tag[i]; i++) line[o++] = tag[i];
        u64 v = (u64)&hook_gate;
        char tmp[24]; int t = 0;
        if (!v) tmp[t++] = '0';
        while (v && t < 20) { unsigned d = (unsigned)(v & 15); tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
        while (t) line[o++] = tmp[--t];
        line[o++] = 10; line[o] = 0;
        int hf = (int)sys5(SYS_openat, (u64)AT_FDCWD, (u64)"/tmp/modgame.hookinfo",
                           O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
        if (hf >= 0) {
            sys3(SYS_write, (u64)hf, (u64)line, (u64)o);
            sys3(SYS_close, (u64)hf, 0, 0);
        }
        klogs("MODGAME: gate published, awaiting external poke\n");
    }
}

static const char FAKE_CMDLINE[] =
    "console=ttyS0,115200 androidboot.hw_version=06.04.01.04 "
    "androidboot.hardware=eagle2 splashboot=hb722 rootwait ro\n";

static int g_inited;
static void lazy_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    g_base = find_base_maps();
    g_qt = g_base;
    if (g_base) klogs("MODGAME: maps base ok\n");
    else klogs("MODGAME: maps FAIL\n");
    install_hook();
}

void *fopen(const char *path, const char *mode)
{
    lazy_init();
    if (path) {
        const char *k = "/proc/cmdline";
        u64 i = 0;
        while (k[i] && path[i] == k[i]) i++;
        if (k[i] == 0 && path[i] == 0)
            return fmemopen_(FAKE_CMDLINE, sizeof FAKE_CMDLINE - 1, "r");
    }
    if (!path || !mode) return 0;
    int flags, plus = 0;
    for (u64 i = 0; mode[i]; i++)
  
        if (mode[i] == '+') plus = 1;
    if (mode[0] == 'r') flags = plus ? O_RDWR_ : O_RDONLY_;
    else if (mode[0] == 'w') flags = O_WRONLY_ | O_CREAT_ | O_TRUNC_ | (plus ? O_RDWR_ : 0);
    else if (mode[0] == 'a') flags = O_WRONLY_ | O_CREAT_ | (plus ? O_RDWR_ : 0);
    else return 0;
    int fd = (int)sys5(SYS_openat, (u64)AT_FDCWD, (u64)path, (u64)flags, 0644);
    if (fd < 0) return 0;
    void *f = fdopen_(fd, mode);
    if (!f) sys3(SYS_close, (u64)fd, 0, 0);
    return f;
}

__attribute__((constructor)) static void modgame_init(void)
{
    klogs("MODGAME: ctor\n");
}
