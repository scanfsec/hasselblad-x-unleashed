/* hginject.c - Hasselblad photo GPS injector / reverter (3FR / JPG / HEIC).
 *
 * hasselblad_gps port of the 10-08 device-verified x2gps/geoinject.c, with
 * the GPS IFD tag set upgraded to the X1D/X1D II lifecycle spec
 * (docs/GPS_LIFECYCLE_X1D_X1DII.md section 3.4) and HEIC support added per
 * docs/X2DII_PORT_ANALYSIS.md section 3 (Exif item 32KB extent tail-slack
 * in-place injection, no box/mdat/iloc change).
 *
 * Formats & strategies (all IFD offsets are TIFF-relative; no byte range the
 * camera wrote ever moves - append + single pointer repoint everywhere):
 *
 *  3FR (TIFF II/MM):
 *    inject : append at EOF [copy of existing Exif IFD + 0x8825][GPS IFD]
 *             [12B trailer]; repoint the IFD0 (and SubIFD, if any) 0x8769
 *             value(s) to the copy.  (verified geoinject scheme; strip/JLS
 *             data untouched)
 *    revert : trailer -> exact original size + old 0x8769 targets; falls
 *             back to the geoinject structural scan for legacy tails.
 *
 *  JPG:
 *    1. EXIF APP1 present  -> append at TIFF end [copy of IFD0 + 0x8825]
 *       [GPS IFD][trailer], grow the APP1 segment (shift file tail), repoint
 *       the TIFF header IFD0 offset.  No TIFF content moves, so MakerNote
 *       internal pointers stay valid.  All IFD offsets are TIFF-relative,
 *       so growing the segment is safe.
 *    2. else XMP APP1 present -> insert an exif:GPS* rdf:Description into
 *       the packet tail padding (segment length unchanged, geoinject
 *       scheme; padding must be uniform so the revert is byte-exact).
 *    3. else -> create a fresh minimal XMP APP1 after SOI/APP0.
 *
 *  HEIC (ftyp box):
 *    locate the Exif item via meta/iinf/iloc -> 32KB extent ->
 *    u32-be-offset + "Exif\0\0" + TIFF; append [copy of IFD0 + 0x8825]
 *    [GPS IFD][trailer] into the zero tail slack of the extent and repoint
 *    the TIFF header IFD0 offset.  Requires all slack bytes to be zero
 *    (checked); exit 5 if the slack is too small.  No box length, mdat or
 *    iloc byte is touched.
 *
 * Idempotency: an existing 0x8825 (or XMP exif:GPSLatitude) => skip, exit 1;
 * --force = revert first, then inject again.
 *
 * Trailer (own-injection marker, little-endian fields, always 12 bytes):
 *    'H','G','T','1', u32 a, u32 b
 *      3FR          : a = original file size, b = original Exif IFD offset
 *      JPG EXIF     : a = original IFD0 offset, b = original APP1 length
 *      JPG XMP-new  : a = 3 (marker), b = 0
 *      HEIC         : a = original IFD0 offset, b = 0
 * Parsers never read it (the GPS IFD next-pointer is 0, ending the chain).
 *
 * Exit codes:
 *    0 OK  1 skipped (already injected / nothing to revert)
 *    2 usage  3 I/O error  4 format/structure error  5 no room
 *
 * C99, no third-party deps.  Write strategy: in-place pread/pwrite + fsync
 * (geoinject convention - a 213MB 3FR must not be copied on the camera
 * exFAT SSD; the trailer makes a half-written tail detectable and the
 * operation re-runnable).  Host + aarch64 Android (NDK r25b).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define HG_OK       0
#define HG_SKIP     1
#define HG_USAGE    2
#define HG_IO       3
#define HG_FORMAT   4
#define HG_NOSPACE  5

#define TRAILER_MAGIC "HGT1"
#define TRAILER_LEN   12

/* ------------------------------------------------------------------ */
/* options                                                             */

typedef struct {
    double lat, lon;            /* required */
    double alt;   int has_alt;
    double dop;   int has_dop;  /* HDOP */
    double speed; int has_speed;/* km/h (EXIF GPSSpeedRef = "K") */
    double track; int has_track;/* degrees true */
    long   nsat;  int has_nsat;
    int    fix_valid;           /* 1 -> GPSStatus 'A', 0 -> 'V' */
    int    yy, mo, dd, hh, mi, ss; int has_utc;
    char   datum[40];
} hg_opt;

static void opt_defaults(hg_opt *o)
{
    memset(o, 0, sizeof *o);
    o->fix_valid = 1;
    strcpy(o->datum, "WGS-84");
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static u16 rd16(const u8 *p, int le)
{
    return (u16)(le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]));
}
static u32 rd32(const u8 *p, int le)
{
    return le ? ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24))
              : (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3]);
}
static void wr16(u8 *p, u16 v, int le)
{
    if (le) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
    else    { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
}
static void wr32(u8 *p, u32 v, int le)
{
    if (le) { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
    else    { p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }
}
static void wr32le(u8 *p, u32 v)
{
    p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24);
}
static u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u64 be64(const u8 *p) { return ((u64)be32(p) << 32) | (u64)be32(p + 4); }

static int tsize(int t)
{
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    }
    return 0;
}

/* portable memmem */
static const u8 *find_blob(const u8 *h, size_t hl, const void *nd, size_t nl)
{
    if (nl == 0 || hl < nl) return NULL;
    const u8 *n = (const u8 *)nd;
    for (size_t i = 0; i + nl <= hl; i++)
        if (h[i] == n[0] && (nl == 1 || !memcmp(h + i, n, nl)))
            return h + i;
    return NULL;
}

static off_t fsize_of(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return (off_t)-1;
    return st.st_size;
}

static int pread_full(int fd, void *b, size_t n, off_t off)
{
    u8 *p = (u8 *)b;
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return HG_IO;
        p += r; off += r; n -= (size_t)r;
    }
    return HG_OK;
}
static int pwrite_full(int fd, const void *b, size_t n, off_t off)
{
    const u8 *p = (const u8 *)b;
    while (n) {
        ssize_t r = pwrite(fd, p, n, off);
        if (r <= 0) return HG_IO;
        p += r; off += r; n -= (size_t)r;
    }
    return HG_OK;
}

/* move file range [start,end) by delta (>0 right, <0 left); caller syncs */
static int file_shift(int fd, off_t start, off_t end, off_t delta)
{
    if (delta == 0 || end <= start) return HG_OK;
    size_t csz = 1u << 20;
    u8 *buf = malloc(csz);
    if (!buf) return HG_IO;
    int rc = HG_OK;
    if (delta > 0) {                              /* backward */
        off_t pos = end;
        while (pos > start) {
            off_t chunk = pos - start;
            if (chunk > (off_t)csz) chunk = (off_t)csz;
            pos -= chunk;
            rc = pread_full(fd, buf, (size_t)chunk, pos);
            if (rc != HG_OK) break;
            rc = pwrite_full(fd, buf, (size_t)chunk, pos + delta);
            if (rc != HG_OK) break;
        }
    } else {                                      /* forward */
        off_t d = -delta;
        off_t pos = start;
        while (pos < end) {
            off_t chunk = end - pos;
            if (chunk > (off_t)csz) chunk = (off_t)csz;
            rc = pread_full(fd, buf, (size_t)chunk, pos);
            if (rc != HG_OK) break;
            rc = pwrite_full(fd, buf, (size_t)chunk, pos - d);
            if (rc != HG_OK) break;
            pos += chunk;
        }
    }
    free(buf);
    return rc;
}

static void wr_trailer(u8 *t, u32 a, u32 b)
{
    memcpy(t, TRAILER_MAGIC, 4);
    wr32le(t + 4, a);
    wr32le(t + 8, b);
}
static int rd_trailer(const u8 *t, u32 *a, u32 *b)
{
    if (memcmp(t, TRAILER_MAGIC, 4)) return 0;
    *a = rd32le(t + 4);
    *b = rd32le(t + 8);
    return 1;
}

/* TIFF-lite IFD view over a buffer */
typedef struct { const u8 *b; size_t blen; int le; } tview;

static int ifd_at(const tview *t, u32 off, u16 *cnt)
{
    if ((u64)off + 2 > t->blen) return HG_FORMAT;
    u16 n = rd16(t->b + off, t->le);
    if (n > 128) return HG_FORMAT;
    if ((u64)off + 2 + (u64)n * 12 + 4 > t->blen) return HG_FORMAT;
    *cnt = n;
    return HG_OK;
}
static const u8 *ent_at(const tview *t, u32 off, int i)
{
    return t->b + off + 2 + (size_t)i * 12;
}

/* ------------------------------------------------------------------ */
/* GPS IFD block (lifecycle-spec tag set)                              */

typedef struct { u32 n, d; } rat;

static rat frac_rat(double v)   /* |v| < 60 at 1e6 precision - geoinject verified */
{
    double x = v * 1000000.0;
    long num = (long)(x > 0 ? x + 0.5 : x - 0.5);
    u32 den = 1000000;
    u32 a = (u32)(num < 0 ? -num : num), b = den;
    rat r;
    if (num == 0) { r.n = 0; r.d = 1; return r; }
    while (b) { u32 t = a % b; a = b; b = t; }
    r.n = (u32)(num / (long)a);
    r.d = den / a;
    return r;
}
static rat int_rat(long v) { rat r; r.n = (u32)v; r.d = 1; return r; }

/*
 * Build a GPS IFD (count + entries + next + values), tags ascending:
 *   always : 0x0000 2.3.0.0, 0x0001/02 lat, 0x0003/04 lon, 0x0007 TimeStamp,
 *            0x0009 Status(A/V), 0x000A MeasureMode "2", 0x0012 MapDatum,
 *            0x001D DateStamp, 0x001E Differential 0
 *   if data : 0x0005/06 altitude, 0x0008 satellites, 0x000B DOP,
 *            0x000C/0D speed (km/h, ref K), 0x000E/0F track (ref T)
 * `base` = absolute (TIFF-header-relative) offset where the block will live;
 * out-of-line value offsets are written base-relative because the EXIF
 * standard resolves them against the TIFF header, not the IFD.
 * Returns block length, or -1 if it does not fit into cap.
 */
static int build_gps(u8 *out, size_t cap, const hg_opt *o, int le, u32 base)
{
    double la = o->lat < 0 ? -o->lat : o->lat;
    double lo = o->lon < 0 ? -o->lon : o->lon;
    double degf = (double)(int)la, minf = (double)(int)((la - degf) * 60);
    double secf = ((la - degf) * 60 - minf) * 60;
    double degl = (double)(int)lo, minl = (double)(int)((lo - degl) * 60);
    double secl = ((lo - degl) * 60 - minl) * 60;
    rat rl[3] = { int_rat((long)degf), frac_rat(minf), frac_rat(secf) };
    rat rr[3] = { int_rat((long)degl), frac_rat(minl), frac_rat(secl) };
    rat rt[3] = { int_rat(o->hh), int_rat(o->mi), int_rat(o->ss) };
    rat ra = frac_rat(o->has_alt && o->alt < 0 ? -o->alt : o->alt);
    char ds[12], ss[8];
    int dsl, ssl;

    struct { u16 tag, typ; u32 cnt; int ilen; u8 inl[4]; size_t voff, vlen; } e[24];
    int n = 0;
    u8 vb[640];
    size_t vl = 0;
    memset(e, 0, sizeof e);

#define E_START(_tg, _ty, _ct) (e[n].tag = (u16)(_tg), e[n].typ = (u16)(_ty), e[n].cnt = (u32)(_ct))
#define E_INL(nb)  (e[n].ilen = (nb), n++)
#define E_VPUT(src, nb) do { \
        if ((nb) <= 4) { memcpy(e[n].inl, (src), (nb)); e[n].ilen = (nb); } \
        else { if (vl & 1) vb[vl++] = 0; \
               e[n].voff = vl; e[n].vlen = (nb); \
               memcpy(vb + vl, (src), (nb)); vl += (nb); e[n].ilen = 0; } \
        n++; \
    } while (0)
    /* values that need endian-aware assembly go through vb directly */
#define V_ALIGN()   do { if (vl & 1) vb[vl++] = 0; } while (0)

    E_START(0x0000, 1, 4);
    { static const u8 ver[4] = { 2, 3, 0, 0 };
      memcpy(e[n].inl, ver, 4); }
    E_INL(4);

    E_START(0x0001, 2, 2);
    e[n].inl[0] = (u8)(o->lat >= 0 ? 'N' : 'S');
    E_INL(2);

    E_START(0x0002, 5, 3);
    V_ALIGN(); e[n].voff = vl; e[n].vlen = 24;
    for (int k = 0; k < 3; k++) { wr32(vb + vl, rl[k].n, le); wr32(vb + vl + 4, rl[k].d, le); vl += 8; }
    e[n].ilen = 0; n++;

    E_START(0x0003, 2, 2);
    e[n].inl[0] = (u8)(o->lon >= 0 ? 'E' : 'W');
    E_INL(2);

    E_START(0x0004, 5, 3);
    V_ALIGN(); e[n].voff = vl; e[n].vlen = 24;
    for (int k = 0; k < 3; k++) { wr32(vb + vl, rr[k].n, le); wr32(vb + vl + 4, rr[k].d, le); vl += 8; }
    e[n].ilen = 0; n++;

    if (o->has_alt) {
        E_START(0x0005, 1, 1);
        e[n].inl[0] = (u8)(o->alt >= 0 ? 0 : 1);
        E_INL(1);
        E_START(0x0006, 5, 1);
        V_ALIGN(); e[n].voff = vl; e[n].vlen = 8;
        wr32(vb + vl, ra.n, le); wr32(vb + vl + 4, ra.d, le); vl += 8;
        e[n].ilen = 0; n++;
    }

    E_START(0x0007, 5, 3);
    V_ALIGN(); e[n].voff = vl; e[n].vlen = 24;
    for (int k = 0; k < 3; k++) { wr32(vb + vl, rt[k].n, le); wr32(vb + vl + 4, rt[k].d, le); vl += 8; }
    e[n].ilen = 0; n++;

    if (o->has_nsat) {
        ssl = snprintf(ss, sizeof ss, "%ld", o->nsat);
        if (ssl <= 0 || (size_t)ssl + 1 >= sizeof ss) return -1;
        E_START(0x0008, 2, (u32)ssl + 1);
        E_VPUT(ss, (size_t)ssl + 1);
    }

    E_START(0x0009, 2, 2);
    e[n].inl[0] = (u8)(o->fix_valid ? 'A' : 'V');
    E_INL(2);

    E_START(0x000A, 2, 2);
    e[n].inl[0] = '2';
    E_INL(2);

    if (o->has_dop) {
        rat rd = frac_rat(o->dop);
        E_START(0x000B, 5, 1);
        V_ALIGN(); e[n].voff = vl; e[n].vlen = 8;
        wr32(vb + vl, rd.n, le); wr32(vb + vl + 4, rd.d, le); vl += 8;
        e[n].ilen = 0; n++;
    }
    if (o->has_speed) {
        rat rd = frac_rat(o->speed);
        E_START(0x000C, 2, 2);
        e[n].inl[0] = 'K';
        E_INL(2);
        E_START(0x000D, 5, 1);
        V_ALIGN(); e[n].voff = vl; e[n].vlen = 8;
        wr32(vb + vl, rd.n, le); wr32(vb + vl + 4, rd.d, le); vl += 8;
        e[n].ilen = 0; n++;
    }
    if (o->has_track) {
        rat rd = frac_rat(o->track);
        E_START(0x000E, 2, 2);
        e[n].inl[0] = 'T';
        E_INL(2);
        E_START(0x000F, 5, 1);
        V_ALIGN(); e[n].voff = vl; e[n].vlen = 8;
        wr32(vb + vl, rd.n, le); wr32(vb + vl + 4, rd.d, le); vl += 8;
        e[n].ilen = 0; n++;
    }

    E_START(0x0012, 2, (u32)strlen(o->datum) + 1);
    E_VPUT(o->datum, strlen(o->datum) + 1);

    dsl = snprintf(ds, sizeof ds, "%04d:%02d:%02d", o->yy, o->mo, o->dd);
    if (dsl != 10) return -1;
    E_START(0x001D, 2, 11);
    E_VPUT(ds, 11);

    E_START(0x001E, 3, 1);
    e[n].inl[0] = 0; e[n].inl[1] = 0;      /* SHORT 0 */
    E_INL(2);
#undef E_START
#undef E_INL
#undef E_VPUT
#undef V_ALIGN

    size_t hdr = 2 + (size_t)n * 12 + 4;
    if (hdr + vl > cap) return -1;
    wr16(out, (u16)n, le);
    u8 *p = out + 2;
    u32 vbase = (u32)hdr;
    for (int i = 0; i < n; i++) {
        wr16(p, e[i].tag, le);
        wr16(p + 2, e[i].typ, le);
        wr32(p + 4, e[i].cnt, le);
        if (e[i].ilen) {
            memset(p + 8, 0, 4);
            memcpy(p + 8, e[i].inl, (size_t)e[i].ilen);
        } else if (e[i].vlen <= 4) {
            /* EXIF: values that fit in 4 bytes are stored inline */
            memset(p + 8, 0, 4);
            memcpy(p + 8, vb + e[i].voff, e[i].vlen);
        } else {
            wr32(p + 8, base + vbase + (u32)e[i].voff, le);
        }
        p += 12;
    }
    wr32(out + 2 + (size_t)n * 12, 0, le);            /* next IFD = 0 */
    memcpy(out + hdr, vb, vl);                        /* value area (pads included) */
    return (int)(hdr + vl);
}

/*
 * Own-injection fingerprint of a GPS IFD found at absolute (TIFF-relative)
 * offset `gstart`; `b` points at the IFD, `max` bytes are available after b.
 * Out-of-line value offsets are absolute, like build_gps writes them.
 * Returns 1 and sets *blen to the exact block length (relative to the IFD).
 */
static int gps_is_ours(const u8 *b, size_t max, int le, u32 gstart, int *blen)
{
    if (max < 8) return 0;
    int n = rd16(b, le);
    if (n < 11 || n > 24) return 0;
    size_t need = 2 + (size_t)n * 12 + 4;
    if (need > max) return 0;
    int has0 = 0, has2 = 0, has4 = 0, has9 = 0, hasA = 0, has12 = 0, has1E = 0;
    u32 prev = 0xFFFFFFFFu;
    u64 vend = 0;
    for (int i = 0; i < n; i++) {
        const u8 *e = b + 2 + (size_t)i * 12;
        u16 tag = rd16(e, le), typ = rd16(e + 2, le);
        u32 cnt = rd32(e + 4, le);
        if (i && tag <= prev) return 0;
        if (tag > 0x001E) return 0;
        prev = tag;
        int ts = tsize(typ);
        if (!ts) return 0;
        u64 sz = (u64)ts * cnt;
        if (sz <= 4) {
            if (tag == 0x0000 && typ == 1 && cnt == 4 &&
                e[8] == 2 && e[9] == 3 && e[10] == 0 && e[11] == 0) has0 = 1;
            if (tag == 0x0009 && typ == 2 && cnt == 2 && (e[8] == 'A' || e[8] == 'V')) has9 = 1;
            if (tag == 0x000A && typ == 2 && cnt == 2 && e[8] == '2') hasA = 1;
            if (tag == 0x001E && typ == 3 && cnt == 1 && rd16(e + 8, le) == 0) has1E = 1;
        } else {
            u32 v = rd32(e + 8, le);
            if (v < gstart) return 0;                       /* must be absolute */
            u64 rel = (u64)v - gstart;
            if (rel + sz > max) return 0;
            if (rel + sz > vend) vend = rel + sz;
            if (tag == 0x0002 && typ == 5 && cnt == 3) has2 = 1;
            if (tag == 0x0004 && typ == 5 && cnt == 3) has4 = 1;
            if (tag == 0x0012 && typ == 2 && cnt >= 6 && rel + 6 <= max &&
                !memcmp(b + rel, "WGS-84", 6)) has12 = 1;
        }
    }
    if (rd32(b + 2 + (size_t)n * 12, le) != 0) return 0;
    if (!(has0 && has2 && has4 && has9 && hasA && has12 && has1E)) return 0;
    if (blen) *blen = (int)vend;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3FR                                                                  */

static u8 head3[0x10000];

static int head3_load(int fd, size_t *hlen)
{
    off_t fs = fsize_of(fd);
    if (fs < 16) return HG_FORMAT;
    *hlen = fs < (off_t)sizeof head3 ? (size_t)fs : sizeof head3;
    return pread_full(fd, head3, *hlen, 0);
}

static int tiff_magic_le(const u8 *b, size_t n)
{
    if (n < 4) return -1;
    if (!memcmp(b, "II\x2a\x00", 4)) return 1;
    if (!memcmp(b, "MM\x00\x2a", 4)) return 0;
    return -1;
}

/* does the Exif IFD at exif_off (TIFF-relative == file offset in 3FR) contain
 * 0x8825?  follows the pointer even into an appended EOF tail outside the
 * header window.  1 = yes, 0 = no, -1 = unreadable */
static int exif_ifd_has_8825(int fd, const tview *head, u32 exif_off, int le)
{
    u16 ne;
    u8 tailb[2 + 60 * 12 + 4];
    const u8 *base;
    if ((u64)exif_off + 2 + 60 * 12 + 4 <= head->blen) {
        base = head->b;
        if (ifd_at(head, exif_off, &ne) != HG_OK) return -1;
    } else {
        off_t fs = fsize_of(fd);
        if (fs < 0x1000 || (u64)exif_off >= (u64)fs) return -1;
        if ((u64)fs - exif_off > 0x4000) return -1;      /* implausible tail */
        if (pread_full(fd, tailb, (size_t)(fs - exif_off), (off_t)exif_off) != HG_OK) return -1;
        {
            tview tv = { tailb, (size_t)(fs - exif_off), le };
            if (ifd_at(&tv, 0, &ne) != HG_OK) return -1;
        }
        base = tailb;
        exif_off = 0;
    }
    for (int i = 0; i < ne; i++)
        if (rd16(base + exif_off + 2 + (size_t)i * 12, le) == 0x8825) return 1;
    return 0;
}

static int inject_3fr(const char *path, const hg_opt *o)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    size_t hlen;
    int rc = head3_load(fd, &hlen);
    if (rc != HG_OK) { close(fd); fprintf(stderr, "hginject: %s: read header\n", path); return rc; }
    int le = tiff_magic_le(head3, hlen);
    if (le < 0) { close(fd); fprintf(stderr, "hginject: %s: not a TIFF/3FR\n", path); return HG_FORMAT; }

    tview tv = { head3, hlen, le };
    u32 ifd0 = rd32(head3 + 4, le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK) { close(fd); fprintf(stderr, "hginject: %s: bad IFD0\n", path); return HG_FORMAT; }
    u32 exif_off = 0, sub_off = 0, sub_cnt = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        u16 tag = rd16(e, le);
        if (tag == 0x8769) exif_off = rd32(e + 8, le);
        if (tag == 0x014a) { sub_off = rd32(e + 8, le); sub_cnt = rd32(e + 4, le); }
    }
    if (!exif_off) { close(fd); fprintf(stderr, "hginject: %s: no 0x8769 Exif IFD\n", path); return HG_FORMAT; }

    /* idempotency: 0x8769 may already point into an injected EOF tail */
    {
        int has = exif_ifd_has_8825(fd, &tv, exif_off, le);
        if (has < 0) { close(fd); fprintf(stderr, "hginject: %s: bad/remote Exif IFD\n", path); return HG_FORMAT; }
        if (has) {
            close(fd);
            fprintf(stderr, "hginject: %s: skipped (0x8825 already present)\n", path);
            return HG_SKIP;
        }
    }

    u16 ne;
    if (ifd_at(&tv, exif_off, &ne) != HG_OK || ne > 60) {
        close(fd); fprintf(stderr, "hginject: %s: bad/remote Exif IFD\n", path); return HG_FORMAT;
    }

    off_t fs = fsize_of(fd);
    if (fs < 0x1000) { close(fd); return HG_FORMAT; }
    u32 pad = (u32)((4 - ((u64)fs & 3)) & 3);
    u32 new_exif = (u32)fs + pad;
    u32 gps_off = new_exif + 2 + (u32)(ne + 1) * 12 + 4;

    u8 tail[8192];
    u8 *bp = tail;
    wr16(bp, (u16)(ne + 1), le); bp += 2;
    int inserted = 0;
    for (int i = 0; i < ne; i++) {
        const u8 *e = ent_at(&tv, exif_off, i);
        u16 tag = rd16(e, le);
        if (!inserted && tag > 0x8825) {
            wr16(bp, 0x8825, le); wr16(bp + 2, 4, le); wr32(bp + 4, 1, le); wr32(bp + 8, gps_off, le);
            bp += 12; inserted = 1;
        }
        memcpy(bp, e, 12); bp += 12;
    }
    if (!inserted) {
        wr16(bp, 0x8825, le); wr16(bp + 2, 4, le); wr32(bp + 4, 1, le); wr32(bp + 8, gps_off, le);
        bp += 12;
    }
    wr32(bp, 0, le); bp += 4;                        /* next IFD */

    int gl = build_gps(bp, sizeof tail - (size_t)(bp - tail) - TRAILER_LEN, o, le, gps_off);
    if (gl < 0) { close(fd); fprintf(stderr, "hginject: %s: GPS block too large\n", path); return HG_FORMAT; }
    bp += gl;
    wr_trailer(bp, (u32)fs, exif_off); bp += TRAILER_LEN;

    static const u8 z[4] = { 0, 0, 0, 0 };
    if (pad && (rc = pwrite_full(fd, z, pad, fs)) != HG_OK) { close(fd); fprintf(stderr, "hginject: %s: append pad\n", path); return rc; }
    if ((rc = pwrite_full(fd, tail, (size_t)(bp - tail), fs + (off_t)pad)) != HG_OK) {
        close(fd); fprintf(stderr, "hginject: %s: append tail\n", path); return rc;
    }

    u8 ob[4]; wr32(ob, new_exif, le);
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        if (rd16(e, le) == 0x8769 && rd32(e + 8, le) == exif_off)
            if ((rc = pwrite_full(fd, ob, 4, (off_t)(ifd0 + 2 + (u64)i * 12 + 8))) != HG_OK) { close(fd); return rc; }
    }
    if (sub_off && sub_cnt == 1) {
        u16 ns;
        if (ifd_at(&tv, sub_off, &ns) == HG_OK)
            for (int i = 0; i < ns; i++) {
                const u8 *e = ent_at(&tv, sub_off, i);
                if (rd16(e, le) == 0x8769 && rd32(e + 8, le) == exif_off)
                    if ((rc = pwrite_full(fd, ob, 4, (off_t)(sub_off + 2 + (u64)i * 12 + 8))) != HG_OK) { close(fd); return rc; }
            }
    }
    if (fsync(fd) != 0) { close(fd); fprintf(stderr, "hginject: %s: fsync: %s\n", path, strerror(errno)); return HG_IO; }
    close(fd);
    printf("hginject: %s: 3FR injected (+%llu bytes, ExifIFD copy @0x%x, GPS IFD @0x%x)\n",
           path, (unsigned long long)(pad + (u64)(bp - tail)), new_exif, gps_off);
    return HG_OK;
}

static int revert_3fr(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    size_t hlen;
    int rc = head3_load(fd, &hlen);
    if (rc != HG_OK) { close(fd); return rc; }
    int le = tiff_magic_le(head3, hlen);
    if (le < 0) { close(fd); return HG_FORMAT; }

    tview tv = { head3, hlen, le };
    u32 ifd0 = rd32(head3 + 4, le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK) { close(fd); return HG_FORMAT; }
    u32 exif_new = 0, sub_off = 0, sub_cnt = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        u16 tag = rd16(e, le);
        if (tag == 0x8769) exif_new = rd32(e + 8, le);
        if (tag == 0x014a) { sub_off = rd32(e + 8, le); sub_cnt = rd32(e + 4, le); }
    }
    off_t fs = fsize_of(fd);
    if (!exif_new || exif_new < 0x1000 || (off_t)exif_new > fs || fs - (off_t)exif_new > 0x4000) {
        close(fd);
        fprintf(stderr, "hginject: %s: nothing to revert (no injected tail)\n", path);
        return HG_SKIP;
    }

    size_t tl_len = (size_t)(fs - exif_new);
    u8 *tail = malloc(tl_len);
    if (!tail) { close(fd); return HG_IO; }
    if ((rc = pread_full(fd, tail, tl_len, (off_t)exif_new)) != HG_OK) { free(tail); close(fd); return rc; }

    tview ev = { tail, tl_len, le };
    u16 ne;
    if (ifd_at(&ev, 0, &ne) != HG_OK || ne < 2 || ne > 60) {
        free(tail); close(fd);
        fprintf(stderr, "hginject: %s: tail is not an injected Exif IFD\n", path);
        return HG_FORMAT;
    }
    u32 gps_abs = 0;
    for (int i = 0; i < ne; i++) {
        const u8 *e = ent_at(&ev, 0, i);
        if (rd16(e, le) == 0x8825) { gps_abs = rd32(e + 8, le); break; }
    }
    /* 0x8825 value is an absolute TIFF (=file) offset; the IFD copy starts at
     * exif_new, so its position inside the tail buffer is gps_abs - exif_new */
    if (!gps_abs || gps_abs < exif_new + 2 + (u32)ne * 12 + 4 ||
        (size_t)(gps_abs - exif_new) >= tl_len) {
        free(tail); close(fd);
        fprintf(stderr, "hginject: %s: nothing to revert (no 0x8825 in tail)\n", path);
        return HG_SKIP;
    }
    size_t gps_rel = (size_t)(gps_abs - exif_new);
    int glen = 0;
    if (!gps_is_ours(tail + gps_rel, tl_len - gps_rel, le, gps_abs, &glen)) {
        free(tail); close(fd);
        fprintf(stderr, "hginject: %s: GPS IFD present but not injected by hginject; refusing\n", path);
        return HG_SKIP;
    }

    /* exact path via trailer, else legacy geoinject structural scan */
    u32 old_exif = 0, orig_size = 0;
    int exact = 0;
    if ((size_t)glen + TRAILER_LEN <= tl_len - gps_rel &&
        rd_trailer(tail + gps_rel + glen, &orig_size, &old_exif) &&
        old_exif >= 0x40 && old_exif < exif_new &&
        (u64)orig_size <= (u64)exif_new && exif_new - orig_size <= 3 &&
        (u64)fs - orig_size == (u64)(exif_new - orig_size) + tl_len) {
        u16 nold;
        if (ifd_at(&tv, old_exif, &nold) == HG_OK && nold == ne - 1) {
            int k = 0, ok = 1;
            for (int i = 0; i < ne && ok; i++) {
                const u8 *e = ent_at(&ev, 0, i);
                if (rd16(e, le) == 0x8825) continue;
                if (memcmp(e, ent_at(&tv, old_exif, k), 12)) ok = 0;
                k++;
            }
            if (ok && k == nold) exact = 1;
        }
    }
    if (!exact) {
        orig_size = exif_new;                        /* truncate drops just the tail */
        u16 m = (u16)(ne - 1);
        old_exif = 0;
        for (u32 cand = 0x40; cand + 2 + (u32)m * 12 <= hlen; cand += 2) {
            if (rd16(head3 + cand, le) != m) continue;
            int k = 0, ok = 1;
            for (int i = 0; i < ne && ok; i++) {
                const u8 *e = ent_at(&ev, 0, i);
                if (rd16(e, le) == 0x8825) continue;
                if (memcmp(e, head3 + cand + 2 + (size_t)k * 12, 12)) ok = 0;
                k++;
            }
            if (ok && k == m) { old_exif = cand; break; }
        }
        if (!old_exif) {
            free(tail); close(fd);
            fprintf(stderr, "hginject: %s: original Exif IFD not found\n", path);
            return HG_FORMAT;
        }
    }

    u8 ob[4]; wr32(ob, old_exif, le);
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        if (rd16(e, le) == 0x8769 && rd32(e + 8, le) == exif_new)
            if ((rc = pwrite_full(fd, ob, 4, (off_t)(ifd0 + 2 + (u64)i * 12 + 8))) != HG_OK) { free(tail); close(fd); return rc; }
    }
    if (sub_off && sub_cnt == 1) {
        u16 ns;
        if (ifd_at(&tv, sub_off, &ns) == HG_OK)
            for (int i = 0; i < ns; i++) {
                const u8 *e = ent_at(&tv, sub_off, i);
                if (rd16(e, le) == 0x8769 && rd32(e + 8, le) == exif_new)
                    if ((rc = pwrite_full(fd, ob, 4, (off_t)(sub_off + 2 + (u64)i * 12 + 8))) != HG_OK) { free(tail); close(fd); return rc; }
            }
    }
    if (ftruncate(fd, (off_t)orig_size) != 0) {
        free(tail); close(fd);
        fprintf(stderr, "hginject: %s: ftruncate: %s\n", path, strerror(errno));
        return HG_IO;
    }
    if (fsync(fd) != 0) { free(tail); close(fd); return HG_IO; }
    free(tail);
    close(fd);
    printf("hginject: %s: 3FR reverted to %llu bytes (ExifIFD ->0x%x)%s\n",
           path, (unsigned long long)orig_size, old_exif, exact ? " [trailer-exact]" : " [legacy scan]");
    return HG_OK;
}

/* ------------------------------------------------------------------ */
/* JPG                                                                  */

#define XMP_NS "http://ns.adobe.com/xap/1.0/"

/* find APP1 (kind 0 = "Exif\0\0", 1 = XMP NS); returns 1 when found */
static int jpg_find_app1(int fd, int kind, off_t *seg_off, u32 *seg_len)
{
    off_t fs = fsize_of(fd);
    off_t off = 2;
    while (off + 4 <= fs) {
        u8 h[4];
        if (pread_full(fd, h, 4, off) != HG_OK) return 0;
        if (h[0] != 0xFF) return 0;
        u8 m = h[1];
        if (m == 0xDA || m == 0xD9) return 0;                /* scan data / EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { off += 2; continue; }
        u32 l = ((u32)h[2] << 8) | h[3];
        if (l < 2 || off + 2 + (off_t)l > fs) return 0;
        if (m == 0xE1) {
            u8 sig[29];
            size_t want = kind ? 28 : 6;
            if (off + 4 + (off_t)want <= fs && pread_full(fd, sig, want, off + 4) == HG_OK) {
                const char *exp = kind ? XMP_NS : "Exif\0";
                if (!memcmp(sig, exp, want)) { *seg_off = off; *seg_len = l; return 1; }
            }
        }
        off += 2 + (off_t)l;
    }
    return 0;
}

static int build_xmp_desc(char *out, size_t cap, const hg_opt *o, int *dlen)
{
    char timestamp[64] = "";
    if (o->has_utc) {
        int n = snprintf(timestamp, sizeof timestamp,
            " exif:GPSTimeStamp='%04d-%02d-%02dT%02d:%02d:%02dZ'",
            o->yy, o->mo, o->dd, o->hh, o->mi, o->ss);
        if (n < 0 || (size_t)n >= sizeof timestamp) return HG_FORMAT;
    }
    int tl;
    if (o->has_alt)
        tl = snprintf(out, cap,
            "<rdf:Description rdf:about='' xmlns:exif='http://ns.adobe.com/exif/1.0/' "
            "exif:GPSLatitude='%+.6f' exif:GPSLongitude='%+.6f' exif:GPSAltitude='%.0f/1000' "
            "exif:GPSAltitudeRef='%d' exif:GPSMapDatum='%s'%s/>",
            o->lat, o->lon, (o->alt < 0 ? -o->alt : o->alt) * 1000.0,
            o->alt >= 0 ? 0 : 1, o->datum, timestamp);
    else
        tl = snprintf(out, cap,
            "<rdf:Description rdf:about='' xmlns:exif='http://ns.adobe.com/exif/1.0/' "
            "exif:GPSLatitude='%+.6f' exif:GPSLongitude='%+.6f' exif:GPSMapDatum='%s'%s/>",
            o->lat, o->lon, o->datum, timestamp);
    if (tl <= 0 || (size_t)tl >= cap) return HG_FORMAT;
    *dlen = tl;
    return HG_OK;
}

/* APP1 EXIF: append [IFD0 copy + 0x8825][GPS][trailer], grow the segment */
static int inject_jpg_exif(const char *path, int fd, off_t seg_off, u32 seg_len, const hg_opt *o)
{
    size_t plen = seg_len - 2;                       /* payload behind the length field */
    u8 *pl = malloc(plen);
    if (!pl) return HG_IO;
    int rc = pread_full(fd, pl, plen, seg_off + 4);
    if (rc != HG_OK) { free(pl); return rc; }

    const u8 *tiff = pl + 6;                         /* "Exif\0\0" */
    u32 tlen = (u32)plen - 6;
    int le = tiff_magic_le(tiff, tlen);
    if (le < 0 || tlen < 8) { free(pl); fprintf(stderr, "hginject: %s: EXIF APP1 has no TIFF header\n", path); return HG_FORMAT; }

    tview tv = { tiff, tlen, le };
    u32 ifd0 = rd32(tiff + 4, le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK || n0 > 64) { free(pl); fprintf(stderr, "hginject: %s: bad IFD0 in EXIF APP1\n", path); return HG_FORMAT; }
    for (int i = 0; i < n0; i++)
        if (rd16(ent_at(&tv, ifd0, i), le) == 0x8825) {
            free(pl);
            fprintf(stderr, "hginject: %s: skipped (0x8825 already present)\n", path);
            return HG_SKIP;
        }

    u32 pad = tlen & 1;
    u32 ifd_len = 2 + (u32)(n0 + 1) * 12 + 4;
    u32 new_ifd0 = tlen + pad;
    u32 gps_off = new_ifd0 + ifd_len;
    u8 gps[768];
    int gl = build_gps(gps, sizeof gps, o, le, gps_off);
    if (gl < 0) { free(pl); return HG_FORMAT; }
    u32 appended = pad + ifd_len + (u32)gl + TRAILER_LEN;
    if ((u64)seg_len + appended > 65533) {
        free(pl);
        fprintf(stderr, "hginject: %s: EXIF APP1 cannot grow by %u bytes (65533 limit)\n", path, appended);
        return HG_NOSPACE;
    }

    u8 *ap = malloc(appended);
    if (!ap) { free(pl); return HG_IO; }
    u8 *p = ap;
    if (pad) *p++ = 0;
    wr16(p, (u16)(n0 + 1), le); p += 2;
    int inserted = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        u16 tag = rd16(e, le);
        if (!inserted && tag > 0x8825) {
            wr16(p, 0x8825, le); wr16(p + 2, 4, le); wr32(p + 4, 1, le); wr32(p + 8, gps_off, le);
            p += 12; inserted = 1;
        }
        memcpy(p, e, 12); p += 12;
    }
    if (!inserted) {
        wr16(p, 0x8825, le); wr16(p + 2, 4, le); wr32(p + 4, 1, le); wr32(p + 8, gps_off, le);
        p += 12;
    }
    memcpy(p, tiff + ifd0 + 2 + (size_t)n0 * 12, 4); p += 4;      /* next-IFD copy */
    memcpy(p, gps, (size_t)gl); p += gl;
    wr_trailer(p, ifd0, seg_len); p += TRAILER_LEN;

    off_t fs = fsize_of(fd);
    off_t seg_end = seg_off + 2 + (off_t)seg_len;
    if (ftruncate(fd, fs + appended) != 0) { free(ap); free(pl); fprintf(stderr, "hginject: %s: ftruncate: %s\n", path, strerror(errno)); return HG_IO; }
    rc = file_shift(fd, seg_end, fs, (off_t)appended);
    if (rc != HG_OK) { free(ap); free(pl); return rc; }
    rc = pwrite_full(fd, ap, appended, seg_end);
    if (rc != HG_OK) { free(ap); free(pl); return rc; }
    u8 b4[4]; wr32(b4, new_ifd0, le);                              /* header IFD0 ptr */
    rc = pwrite_full(fd, b4, 4, seg_off + 4 + 6 + 4);
    if (rc != HG_OK) { free(ap); free(pl); return rc; }
    u8 b2[2] = { (u8)(((seg_len + appended) >> 8) & 0xFF), (u8)((seg_len + appended) & 0xFF) };
    rc = pwrite_full(fd, b2, 2, seg_off + 2);
    if (rc != HG_OK) { free(ap); free(pl); return rc; }
    if (fsync(fd) != 0) { free(ap); free(pl); return HG_IO; }
    free(ap);
    free(pl);
    printf("hginject: %s: JPG EXIF injected (APP1 %u->%u bytes, IFD0 copy @TIFF+0x%x, GPS IFD @TIFF+0x%x)\n",
           path, seg_len, seg_len + appended, new_ifd0, gps_off);
    return HG_OK;
}

static int revert_jpg_exif(const char *path, int fd, off_t seg_off, u32 seg_len)
{
    if (seg_len < 6 + 8 + 2 + TRAILER_LEN + 12) return HG_SKIP;
    size_t plen = seg_len - 2;
    u8 *pl = malloc(plen);
    if (!pl) return HG_IO;
    int rc = pread_full(fd, pl, plen, seg_off + 4);
    if (rc != HG_OK) { free(pl); return rc; }

    const u8 *tiff = pl + 6;
    u32 tlen = (u32)plen - 6;
    int le = tiff_magic_le(tiff, tlen);
    if (le < 0 || tlen < 8) { free(pl); return HG_FORMAT; }

    tview tv = { tiff, tlen, le };
    u32 ifd0 = rd32(tiff + 4, le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK || n0 > 64 || n0 < 2) { free(pl); return HG_FORMAT; }
    u32 gps_off = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        if (rd16(e, le) == 0x8825) { gps_off = rd32(e + 8, le); break; }
    }
    if (!gps_off) { free(pl); return HG_SKIP; }
    int glen = 0;
    if ((u64)gps_off + 8 > tlen || !gps_is_ours(tiff + gps_off, tlen - gps_off, le, gps_off, &glen)) {
        free(pl);
        fprintf(stderr, "hginject: %s: 0x8825 present but not an hginject tail; refusing\n", path);
        return HG_SKIP;
    }
    u32 old_ifd0 = 0, old_seglen = 0;
    if ((u64)gps_off + glen + TRAILER_LEN > tlen ||
        !rd_trailer(tiff + gps_off + glen, &old_ifd0, &old_seglen) ||
        old_seglen < 8 || old_seglen >= seg_len || old_ifd0 >= gps_off) {
        free(pl);
        fprintf(stderr, "hginject: %s: EXIF tail trailer missing/invalid\n", path);
        return HG_FORMAT;
    }
    /* full geometry cross-check */
    {
        u32 old_tlen = old_seglen - 8;               /* payload minus marker struct */
        u32 pad = old_tlen & 1;
        if (gps_off != old_tlen + pad + 2 + (u32)n0 * 12 + 4 ||
            (u64)gps_off + glen + TRAILER_LEN != tlen) {
            free(pl);
            fprintf(stderr, "hginject: %s: EXIF tail geometry mismatch\n", path);
            return HG_FORMAT;
        }
    }
    /* the old IFD0 must equal the current one minus the 0x8825 entry */
    {
        u16 nold;
        if (ifd_at(&tv, old_ifd0, &nold) != HG_OK || nold != n0 - 1) {
            free(pl); fprintf(stderr, "hginject: %s: original IFD0 mismatch\n", path);
            return HG_FORMAT;
        }
        int k = 0;
        for (int i = 0; i < n0; i++) {
            const u8 *e = ent_at(&tv, ifd0, i);
            if (rd16(e, le) == 0x8825) continue;
            if (memcmp(e, ent_at(&tv, old_ifd0, k), 12)) { free(pl); return HG_FORMAT; }
            k++;
        }
    }

    off_t fs = fsize_of(fd);
    u32 appended = seg_len - old_seglen;
    u8 b4[4]; wr32(b4, old_ifd0, le);
    rc = pwrite_full(fd, b4, 4, seg_off + 4 + 6 + 4);
    if (rc != HG_OK) { free(pl); return rc; }
    u8 b2[2] = { (u8)((old_seglen >> 8) & 0xFF), (u8)(old_seglen & 0xFF) };
    rc = pwrite_full(fd, b2, 2, seg_off + 2);
    if (rc != HG_OK) { free(pl); return rc; }
    off_t seg_end = seg_off + 2 + (off_t)seg_len;
    rc = file_shift(fd, seg_end, fs, -(off_t)appended);
    if (rc != HG_OK) { free(pl); return rc; }
    if (ftruncate(fd, fs - (off_t)appended) != 0) { free(pl); fprintf(stderr, "hginject: %s: ftruncate: %s\n", path, strerror(errno)); return HG_IO; }
    if (fsync(fd) != 0) { free(pl); return HG_IO; }
    free(pl);
    printf("hginject: %s: JPG EXIF reverted (-%u bytes, IFD0 ->TIFF+0x%x)\n", path, appended, old_ifd0);
    return HG_OK;
}

/* XMP path 2: insert the description into existing packet padding */
static int inject_jpg_xmp_pad(const char *path, int fd, off_t seg_off, u32 seg_len, const hg_opt *o)
{
    size_t plen = seg_len - 2;
    u8 *pl = malloc(plen);
    if (!pl) return HG_IO;
    int rc = pread_full(fd, pl, plen, seg_off + 4);
    if (rc != HG_OK) { free(pl); return rc; }

    if (find_blob(pl, plen, "exif:GPSLatitude", 16)) {
        free(pl);
        fprintf(stderr, "hginject: %s: skipped (XMP already carries GPS)\n", path);
        return HG_SKIP;
    }
    const u8 *xr = find_blob(pl + 29, plen - 29, "</rdf:RDF>", 10);
    const u8 *xm = find_blob(pl + 29, plen - 29, "</x:xmpmeta>", 12);
    if (!xr || !xm || xm < xr) { free(pl); fprintf(stderr, "hginject: %s: XMP packet shape unexpected\n", path); return HG_FORMAT; }
    size_t ins_rel = (size_t)(xr - pl);              /* payload-relative */
    size_t pad_start = (size_t)(xm - pl) + 12;
    size_t pad_avail = plen - pad_start;
    char desc[512];
    int dl;
    rc = build_xmp_desc(desc, sizeof desc, o, &dl);
    if (rc != HG_OK) { free(pl); return rc; }
    if (pad_avail < (size_t)dl + 16) {
        free(pl);
        fprintf(stderr, "hginject: %s: XMP padding %zu < needed %d\n", path, pad_avail, dl + 16);
        return HG_NOSPACE;
    }
    /* padding must be uniform so the revert restores it byte-exactly */
    u8 pc = pl[plen - 1];
    for (size_t i = pad_start; i < plen; i++)
        if (pl[i] != pc) {
            free(pl);
            fprintf(stderr, "hginject: %s: XMP padding not uniform (revert would not be exact)\n", path);
            return HG_FORMAT;
        }

    off_t ins_abs = seg_off + 4 + (off_t)ins_rel;
    off_t end_abs = seg_off + 4 + (off_t)plen;
    size_t keep = (size_t)(end_abs - (off_t)dl - ins_abs);
    u8 *tb = malloc(keep ? keep : 1);
    if (!tb) { free(pl); return HG_IO; }
    if (keep && (rc = pread_full(fd, tb, keep, ins_abs)) != HG_OK) { free(tb); free(pl); return rc; }
    if (keep && (rc = pwrite_full(fd, tb, keep, ins_abs + dl)) != HG_OK) { free(tb); free(pl); return rc; }
    rc = pwrite_full(fd, desc, (size_t)dl, ins_abs);
    if (rc != HG_OK) { free(tb); free(pl); return rc; }
    if (fsync(fd) != 0) { free(tb); free(pl); return HG_IO; }
    free(tb);
    free(pl);
    printf("hginject: %s: JPG XMP injected into packet padding (+%d bytes, segment length unchanged)\n",
           path, dl);
    return HG_OK;
}

static int revert_jpg_xmp_pad(const char *path, int fd, off_t seg_off, u32 seg_len)
{
    size_t plen = seg_len - 2;
    if (plen < 64) return HG_SKIP;
    u8 *pl = malloc(plen);
    if (!pl) return HG_IO;
    int rc = pread_full(fd, pl, plen, seg_off + 4);
    if (rc != HG_OK) { free(pl); return rc; }
    const u8 *g = find_blob(pl + 29, plen - 29, "<rdf:Description rdf:about='' xmlns:exif=", 41);
    if (!g) { free(pl); return HG_SKIP; }
    size_t g_rel = (size_t)(g - pl);
    size_t scope = plen - g_rel > 1024 ? 1024 : plen - g_rel;
    const u8 *e = find_blob(g, scope, "/>", 2);
    if (!e || (size_t)(e + 2 - pl) > plen) { free(pl); return HG_FORMAT; }
    size_t ins_rel = g_rel, tl = (size_t)(e + 2 - g);
    off_t ins_abs = seg_off + 4 + (off_t)ins_rel;
    off_t end_abs = seg_off + 4 + (off_t)plen;
    u8 pc = pl[plen - 1];
    size_t keep = end_abs - (off_t)tl - ins_abs > 0 ? (size_t)(end_abs - (off_t)tl - ins_abs) : 0;
    u8 *tb = malloc(keep ? keep : 1);
    if (!tb) { free(pl); return HG_IO; }
    if (keep && (rc = pread_full(fd, tb, keep, ins_abs + (off_t)tl)) != HG_OK) { free(tb); free(pl); return rc; }
    if (keep && (rc = pwrite_full(fd, tb, keep, ins_abs)) != HG_OK) { free(tb); free(pl); return rc; }
    u8 *z = malloc(tl ? tl : 1);
    if (!z) { free(tb); free(pl); return HG_IO; }
    memset(z, pc, tl);
    rc = pwrite_full(fd, z, tl, end_abs - (off_t)tl);
    free(z); free(tb); free(pl);
    if (rc != HG_OK) return rc;
    if (fsync(fd) != 0) return HG_IO;
    printf("hginject: %s: JPG XMP reverted (padding restored, -%zu bytes)\n", path, tl);
    return HG_OK;
}

/* XMP path 3: create a fresh minimal XMP APP1 right after SOI/APP0 */
static int inject_jpg_xmp_new(const char *path, int fd, const hg_opt *o)
{
    char desc[512];
    int dl;
    int rc = build_xmp_desc(desc, sizeof desc, o, &dl);
    if (rc != HG_OK) return rc;

    size_t pad_n = 1024;
    size_t cap = 256 + (size_t)dl + pad_n + TRAILER_LEN + 64;
    u8 *seg = malloc(cap);
    if (!seg) return HG_IO;
    char *w = (char *)seg;
    size_t sl = 0, sp;
    sp = (size_t)snprintf(w + sl, cap - sl,
        "<?xpacket begin='\xef\xbb\xbf' id='W5M0MpCehiHzreSzNTczkc9d'?>\n");
    if (sp >= cap - sl) { free(seg); return HG_FORMAT; }
    sl += sp;
    sp = (size_t)snprintf(w + sl, cap - sl, "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n");
    if (sp >= cap - sl) { free(seg); return HG_FORMAT; }
    sl += sp;
    sp = (size_t)snprintf(w + sl, cap - sl,
        "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n");
    if (sp >= cap - sl) { free(seg); return HG_FORMAT; }
    sl += sp;
    memcpy(seg + sl, desc, (size_t)dl); sl += (size_t)dl;
    sp = (size_t)snprintf(w + sl, cap - sl, "\n</rdf:RDF>\n</x:xmpmeta>\n");
    if (sp >= cap - sl) { free(seg); return HG_FORMAT; }
    sl += sp;
    memset(seg + sl, 0x20, pad_n); sl += pad_n;
    sp = (size_t)snprintf(w + sl, cap - sl, "<?xpacket end='w'?>");
    if (sp >= cap - sl) { free(seg); return HG_FORMAT; }
    sl += sp;
    wr_trailer(seg + sl, 3, 0); sl += TRAILER_LEN;

    size_t content = 29 + sl;                        /* NS(28)+NUL + packet */
    if (content + 2 > 65533) { free(seg); return HG_NOSPACE; }
    u32 lfield = (u32)(content + 2);                 /* length field counts itself */
    u32 total = 2 + 2 + (u32)content;

    off_t ins = 2;                                   /* after SOI, skip APP0 if present */
    {
        u8 h[4];
        if (pread_full(fd, h, 4, 2) == HG_OK && h[0] == 0xFF && h[1] == 0xE0) {
            u32 l = ((u32)h[2] << 8) | h[3];
            if (l >= 2) ins = 2 + 2 + (off_t)l;
        }
    }
    u8 *full = malloc(total);
    if (!full) { free(seg); return HG_IO; }
    full[0] = 0xFF; full[1] = 0xE1;
    full[2] = (u8)((lfield >> 8) & 0xFF); full[3] = (u8)(lfield & 0xFF);
    memcpy(full + 4, XMP_NS, 29);
    memcpy(full + 4 + 29, seg, sl);

    off_t fs = fsize_of(fd);
    if (ftruncate(fd, fs + total) != 0) { free(full); free(seg); fprintf(stderr, "hginject: %s: ftruncate: %s\n", path, strerror(errno)); return HG_IO; }
    rc = file_shift(fd, ins, fs, (off_t)total);
    if (rc != HG_OK) { free(full); free(seg); return rc; }
    rc = pwrite_full(fd, full, total, ins);
    free(full); free(seg);
    if (rc != HG_OK) return rc;
    if (fsync(fd) != 0) return HG_IO;
    printf("hginject: %s: JPG XMP APP1 created at offset %lld (%u bytes incl. %zu padding)\n",
           path, (long long)ins, total, pad_n);
    return HG_OK;
}

/* does the XMP APP1 payload end with our created-packet trailer? */
static int xmp_is_created(int fd, off_t seg_off, u32 seg_len)
{
    if (seg_len < 2 + 29 + TRAILER_LEN + 20) return 0;
    u8 t[TRAILER_LEN];
    off_t end = seg_off + 2 + (off_t)seg_len;        /* payload end */
    if (pread_full(fd, t, TRAILER_LEN, end - TRAILER_LEN) != HG_OK) return 0;
    u32 a, b;
    return rd_trailer(t, &a, &b) && a == 3;
}

static int revert_jpg_xmp_new(const char *path, int fd, off_t seg_off, u32 seg_len)
{
    off_t fs = fsize_of(fd);
    off_t end = seg_off + 2 + (off_t)seg_len;
    int rc = file_shift(fd, end, fs, -(off_t)(2 + (off_t)seg_len));
    if (rc != HG_OK) return rc;
    if (ftruncate(fd, fs - (2 + (off_t)seg_len)) != 0) {
        fprintf(stderr, "hginject: %s: ftruncate: %s\n", path, strerror(errno));
        return HG_IO;
    }
    if (fsync(fd) != 0) return HG_IO;
    printf("hginject: %s: JPG created XMP APP1 removed (-%u bytes)\n", path, seg_len + 2);
    return HG_OK;
}

static int inject_jpg(const char *path, const hg_opt *o)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    off_t so; u32 sl;
    int rc;
    if (jpg_find_app1(fd, 0, &so, &sl))
        rc = inject_jpg_exif(path, fd, so, sl, o);
    else if (jpg_find_app1(fd, 1, &so, &sl))
        rc = inject_jpg_xmp_pad(path, fd, so, sl, o);
    else
        rc = inject_jpg_xmp_new(path, fd, o);
    close(fd);
    return rc;
}

static int revert_jpg(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    off_t so; u32 sl;
    int rc = HG_SKIP;
    if (jpg_find_app1(fd, 0, &so, &sl)) {
        rc = revert_jpg_exif(path, fd, so, sl);
        if (rc == HG_SKIP && jpg_find_app1(fd, 1, &so, &sl)) {
            if (xmp_is_created(fd, so, sl))
                rc = revert_jpg_xmp_new(path, fd, so, sl);
            else
                rc = revert_jpg_xmp_pad(path, fd, so, sl);
        }
        if (rc == HG_SKIP)
            fprintf(stderr, "hginject: %s: nothing to revert\n", path);
    } else if (jpg_find_app1(fd, 1, &so, &sl)) {
        if (xmp_is_created(fd, so, sl))
            rc = revert_jpg_xmp_new(path, fd, so, sl);
        else
            rc = revert_jpg_xmp_pad(path, fd, so, sl);
    } else {
        fprintf(stderr, "hginject: %s: nothing to revert (no EXIF/XMP APP1)\n", path);
    }
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* HEIC                                                                 */

typedef struct { off_t off; u64 len; } ext_t;

static int heic_find_meta(int fd, off_t fs, off_t *meta_off, u64 *meta_size)
{
    off_t off = 0;
    u8 h[16];
    while (off + 8 <= fs) {
        if (pread_full(fd, h, 8, off) != HG_OK) return HG_IO;
        u64 size = be32(h);
        int hdrlen = 8;
        if (size == 1) {
            if (pread_full(fd, h + 8, 8, off + 8) != HG_OK) return HG_IO;
            size = be64(h + 8);
            hdrlen = 16;
        } else if (size == 0) {
            size = (u64)(fs - off);
        }
        if (size < (u64)hdrlen || (u64)off + size > (u64)fs) return HG_FORMAT;
        if (!memcmp(h + 4, "meta", 4)) { *meta_off = off; *meta_size = size; return HG_OK; }
        if (!memcmp(h + 4, "mdat", 4)) return HG_FORMAT;     /* meta must precede mdat */
        off += (off_t)size;
    }
    return HG_FORMAT;
}

/* find the Exif item extent through meta/iinf + meta/iloc (two passes) */
static int heic_exif_extent(int fd, off_t meta_off, u64 meta_size, ext_t *ext)
{
    off_t mend = meta_off + (off_t)meta_size;
    u32 exif_id = 0;
    int have_id = 0, have_ext = 0;
    u8 h[8];

    for (int pass = 0; pass < 2; pass++) {
        off_t coff = meta_off + 8 + 4;               /* box hdr + FullBox ver/flags */
        while (coff + 8 <= mend) {
            if (pread_full(fd, h, 8, coff) != HG_OK) return HG_IO;
            u64 size = be32(h);
            if (size == 0) size = (u64)(mend - coff);
            if (size < 8 || coff + (off_t)size > mend) return HG_FORMAT;
            if (pass == 0 && !memcmp(h + 4, "iinf", 4)) {
                u8 b[8];
                if (pread_full(fd, b, 8, coff + 8) != HG_OK) return HG_IO;
                int ver = b[0];
                off_t p = coff + 12 + (ver == 1 ? 4 : 2);
                off_t pend = coff + (off_t)size;
                while (p + 8 <= pend) {
                    if (pread_full(fd, b, 8, p) != HG_OK) return HG_IO;
                    u64 isz = be32(b);
                    if (isz < 8 || p + (off_t)isz > pend) break;
                    if (!memcmp(b + 4, "infe", 4) && isz >= 18) {
                        u8 ih[8], ii[8];
                        if (pread_full(fd, ih, 8, p + 8) != HG_OK) return HG_IO;
                        if (ih[0] >= 2 && pread_full(fd, ii, 8, p + 12) == HG_OK) {
                            if (!memcmp(ii + 4, "Exif", 4)) {
                                exif_id = ((u32)ii[0] << 8) | ii[1];
                                have_id = 1;
                            }
                        }
                    }
                    p += (off_t)isz;
                }
            } else if (pass == 1 && !memcmp(h + 4, "iloc", 4) && have_id) {
                u8 lb[8];
                if (pread_full(fd, lb, 8, coff + 8) != HG_OK) return HG_IO;
                int ver = lb[0];
                int osz = lb[4] >> 4, lsz = lb[4] & 0xF;
                int bsz = lb[5] >> 4, xsz = lb[5] & 0xF;
                if (osz != 4 || lsz != 4 || bsz > 8 || xsz > 8) return HG_FORMAT;
                u32 icount;
                {
                    u8 cb[4];
                    if (pread_full(fd, cb, ver == 2 ? 4 : 2, coff + 14) != HG_OK) return HG_IO;
                    icount = ver == 2 ? be32(cb) : (u32)(((u32)cb[0] << 8) | cb[1]);
                }
                off_t p = coff + 14 + (ver == 2 ? 4 : 2);
                off_t pend = coff + (off_t)size;
                for (u32 it = 0; it < icount && p + 4 <= pend; it++) {
                    u32 id = 0; int method = 0;
                    u64 base = 0; u32 ecnt = 0;
                    u8 b[8];
                    if (ver < 2) {
                        if (pread_full(fd, b, 2, p) != HG_OK) return HG_IO;
                        id = ((u32)b[0] << 8) | b[1]; p += 2;
                    } else {
                        if (pread_full(fd, b, 4, p) != HG_OK) return HG_IO;
                        id = be32(b); p += 4;
                    }
                    if (ver == 1 || ver == 2) {
                        if (pread_full(fd, b, 2, p) != HG_OK) return HG_IO;
                        method = ((int)b[0] << 8) | b[1]; p += 2;
                    }
                    p += 2;                          /* data_reference_index */
                    if (bsz) {
                        if (pread_full(fd, b, (size_t)bsz, p) != HG_OK) return HG_IO;
                        for (int k = 0; k < bsz; k++) base = (base << 8) | b[k];
                        p += bsz;
                    }
                    if (xsz) p += xsz;               /* extent_index */
                    if (pread_full(fd, b, 2, p) != HG_OK) return HG_IO;
                    ecnt = ((u32)b[0] << 8) | b[1]; p += 2;
                    for (u32 ex = 0; ex < ecnt && p + 8 <= pend; ex++) {
                        u8 ob[8];
                        if (pread_full(fd, ob, 8, p) != HG_OK) return HG_IO;
                        u32 eo = be32(ob), el = be32(ob + 4);
                        p += 8;
                        if (id == exif_id) {
                            if (have_ext) return HG_FORMAT;    /* multiple extents */
                            if (method != 0) return HG_FORMAT; /* idat-embedded Exif */
                            ext->off = (off_t)base + (off_t)eo;
                            ext->len = el;
                            have_ext = 1;
                        }
                    }
                }
            }
            coff += (off_t)size;
        }
    }
    if (!have_id) { fprintf(stderr, "hginject: no Exif item in meta box\n"); return HG_FORMAT; }
    if (!have_ext) { fprintf(stderr, "hginject: Exif item has no file extent\n"); return HG_FORMAT; }
    return HG_OK;
}

/* parse the Exif-item payload (u32-be skip + "Exif\0\0" + TIFF) */
static int exif_payload_tiff(u8 *buf, u64 blen, tview *tv, u32 *trel)
{
    if (blen < 16) return HG_FORMAT;
    u32 skip = be32(buf);
    if ((u64)4 + skip + 8 > blen) return HG_FORMAT;
    u8 *tp = buf + 4 + skip;
    int le = tiff_magic_le(tp, (size_t)(blen - 4 - skip));
    if (le < 0) return HG_FORMAT;
    tv->b = tp;
    tv->blen = (size_t)(blen - 4 - skip);
    tv->le = le;
    *trel = 4 + skip;
    return HG_OK;
}

/* high-water mark of TIFF content (IFD tables + out-of-line values) */
static int tiff_highwater(const tview *tv, u32 off, int depth, u32 *hw)
{
    u16 n;
    if (depth > 4 || ifd_at(tv, off, &n) != HG_OK) return HG_FORMAT;
    u32 end = off + 2 + (u32)n * 12 + 4;
    if (end > *hw) *hw = end;
    for (int i = 0; i < n; i++) {
        const u8 *e = ent_at(tv, off, i);
        u16 tag = rd16(e, tv->le), typ = rd16(e + 2, tv->le);
        u32 cnt = rd32(e + 4, tv->le);
        int ts = tsize(typ);
        if (!ts) continue;
        u64 sz = (u64)ts * cnt;
        if (sz > 4) {
            u32 v = rd32(e + 8, tv->le);
            if ((u64)v + sz > (u64)tv->blen) return HG_FORMAT;
            if (v + (u32)sz > *hw) *hw = v + (u32)sz;
        }
        if (tag == 0x8769 &&
            tiff_highwater(tv, rd32(e + 8, tv->le), depth + 1, hw) != HG_OK)
            return HG_FORMAT;
    }
    u32 nxt = rd32(tv->b + off + 2 + (u64)n * 12, tv->le);
    if (nxt && tiff_highwater(tv, nxt, depth + 1, hw) != HG_OK) return HG_FORMAT;
    return HG_OK;
}

static int heic_open_extent(const char *path, int *fd, ext_t *ext, u8 **buf, tview *tv, u32 *trel)
{
    *fd = open(path, O_RDWR);
    if (*fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    off_t fs = fsize_of(*fd);
    off_t meta_off; u64 meta_size;
    int rc = heic_find_meta(*fd, fs, &meta_off, &meta_size);
    if (rc != HG_OK) { close(*fd); fprintf(stderr, "hginject: %s: no meta box\n", path); return rc; }
    rc = heic_exif_extent(*fd, meta_off, meta_size, ext);
    if (rc != HG_OK) { close(*fd); return rc; }
    if (ext->len == 0 || ext->len > (1u << 20) ||
        (u64)ext->off + ext->len > (u64)fs) { close(*fd); return HG_FORMAT; }
    *buf = malloc((size_t)ext->len);
    if (!*buf) { close(*fd); return HG_IO; }
    rc = pread_full(*fd, *buf, (size_t)ext->len, ext->off);
    if (rc != HG_OK) { free(*buf); close(*fd); return rc; }
    rc = exif_payload_tiff(*buf, ext->len, tv, trel);
    if (rc != HG_OK) {
        free(*buf); close(*fd);
        fprintf(stderr, "hginject: %s: Exif item payload is not a TIFF\n", path);
        return rc;
    }
    return HG_OK;
}

static int inject_heic(const char *path, const hg_opt *o)
{
    int fd; ext_t ext; u8 *buf; tview tv; u32 trel;
    int rc = heic_open_extent(path, &fd, &ext, &buf, &tv, &trel);
    if (rc != HG_OK) return rc;

    u32 ifd0 = rd32(tv.b + 4, tv.le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK || n0 > 64) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: bad IFD0 in Exif item\n", path);
        return HG_FORMAT;
    }
    for (int i = 0; i < n0; i++)
        if (rd16(ent_at(&tv, ifd0, i), tv.le) == 0x8825) {
            free(buf); close(fd);
            fprintf(stderr, "hginject: %s: skipped (0x8825 already present)\n", path);
            return HG_SKIP;
        }

    u32 hw = 8;
    if (tiff_highwater(&tv, ifd0, 0, &hw) != HG_OK) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: TIFF walk failed\n", path);
        return HG_FORMAT;
    }

    u8 gps[768];
    u32 pad = hw & 1;
    u32 ifd_len = 2 + (u32)(n0 + 1) * 12 + 4;
    u32 new_ifd0 = hw + pad;
    u32 gps_off = new_ifd0 + ifd_len;
    int gl = build_gps(gps, sizeof gps, o, tv.le, gps_off);
    if (gl < 0) { free(buf); close(fd); return HG_FORMAT; }
    u32 need = pad + ifd_len + (u32)gl + TRAILER_LEN;
    u64 tspace = ext.len - trel;                     /* TIFF-relative address space */
    if ((u64)hw + need > tspace) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: Exif extent tail slack %llu < needed %u (refusing to rewrite boxes)\n",
                path, (unsigned long long)(tspace - hw), need);
        return HG_NOSPACE;
    }
    for (u64 i = hw; i < tspace; i++)
        if (tv.b[i] != 0) {
            free(buf); close(fd);
            fprintf(stderr, "hginject: %s: extent tail not zero-padded at TIFF+0x%llx; refusing (revert would not be exact)\n",
                    path, (unsigned long long)i);
            return HG_FORMAT;
        }

    u8 *p = buf + trel + new_ifd0;
    if (pad) *p++ = 0;
    wr16(p, (u16)(n0 + 1), tv.le); p += 2;
    int inserted = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        u16 tag = rd16(e, tv.le);
        if (!inserted && tag > 0x8825) {
            wr16(p, 0x8825, tv.le); wr16(p + 2, 4, tv.le); wr32(p + 4, 1, tv.le); wr32(p + 8, gps_off, tv.le);
            p += 12; inserted = 1;
        }
        memcpy(p, e, 12); p += 12;
    }
    if (!inserted) {
        wr16(p, 0x8825, tv.le); wr16(p + 2, 4, tv.le); wr32(p + 4, 1, tv.le); wr32(p + 8, gps_off, tv.le);
        p += 12;
    }
    memcpy(p, tv.b + ifd0 + 2 + (size_t)n0 * 12, 4); p += 4;     /* next-IFD copy */
    memcpy(p, gps, (size_t)gl); p += gl;
    wr_trailer(p, ifd0, 0); p += TRAILER_LEN;
    wr32((u8 *)tv.b + 4, new_ifd0, tv.le);                       /* repoint header */

    rc = pwrite_full(fd, buf, (size_t)ext.len, ext.off);
    if (rc == HG_OK && fsync(fd) != 0) rc = HG_IO;
    free(buf);
    close(fd);
    if (rc != HG_OK) return rc;
    printf("hginject: %s: HEIC injected in-place (extent @0x%llx, IFD0 copy @TIFF+0x%x, GPS IFD @TIFF+0x%x, +%u bytes, no box changed)\n",
           path, (unsigned long long)ext.off, new_ifd0, gps_off, need);
    return HG_OK;
}

static int revert_heic(const char *path)
{
    int fd; ext_t ext; u8 *buf; tview tv; u32 trel;
    int rc = heic_open_extent(path, &fd, &ext, &buf, &tv, &trel);
    if (rc != HG_OK) return rc;

    u32 ifd0 = rd32(tv.b + 4, tv.le);
    u16 n0;
    if (ifd_at(&tv, ifd0, &n0) != HG_OK || n0 > 64 || n0 < 2) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: nothing to revert\n", path);
        return HG_SKIP;
    }
    u32 gps_off = 0;
    for (int i = 0; i < n0; i++) {
        const u8 *e = ent_at(&tv, ifd0, i);
        if (rd16(e, tv.le) == 0x8825) { gps_off = rd32(e + 8, tv.le); break; }
    }
    if (!gps_off) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: nothing to revert (no 0x8825)\n", path);
        return HG_SKIP;
    }
    int glen = 0;
    if ((u64)gps_off + 8 > tv.blen || !gps_is_ours(tv.b + gps_off, tv.blen - gps_off, tv.le, gps_off, &glen)) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: 0x8825 present but not injected by hginject; refusing\n", path);
        return HG_SKIP;
    }
    u32 old_ifd0 = 0, rb = 1;
    if ((u64)gps_off + glen + TRAILER_LEN > tv.blen ||
        !rd_trailer(tv.b + gps_off + glen, &old_ifd0, &rb) || rb != 0 ||
        old_ifd0 >= gps_off ||
        /* the GPS IFD must directly follow the live (appended) IFD0 copy */
        ifd0 + 2 + (u32)n0 * 12 + 4 != gps_off) {
        free(buf); close(fd);
        fprintf(stderr, "hginject: %s: no valid hginject trailer in Exif item; refusing\n", path);
        return HG_SKIP;
    }
    /* the old IFD0 must equal the current one minus the 0x8825 entry */
    {
        u16 nold;
        if (ifd_at(&tv, old_ifd0, &nold) != HG_OK || nold != n0 - 1) {
            free(buf); close(fd);
            fprintf(stderr, "hginject: %s: original IFD0 mismatch\n", path);
            return HG_FORMAT;
        }
        int k = 0;
        for (int i = 0; i < n0; i++) {
            const u8 *e = ent_at(&tv, ifd0, i);
            if (rd16(e, tv.le) == 0x8825) continue;
            if (memcmp(e, ent_at(&tv, old_ifd0, k), 12)) { free(buf); close(fd); return HG_FORMAT; }
            k++;
        }
    }
    wr32((u8 *)tv.b + 4, old_ifd0, tv.le);
    memset((u8 *)tv.b + ifd0, 0, (size_t)(tv.blen - ifd0));      /* slack was all zero */
    rc = pwrite_full(fd, buf, (size_t)ext.len, ext.off);
    if (rc == HG_OK && fsync(fd) != 0) rc = HG_IO;
    free(buf);
    close(fd);
    if (rc != HG_OK) return rc;
    printf("hginject: %s: HEIC reverted (IFD0 ->TIFF+0x%x, extent slack re-zeroed)\n", path, old_ifd0);
    return HG_OK;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */

static void usage(FILE *f)
{
    fprintf(f,
        "usage: hginject <file> --lat F --lon F [--alt F] [--dop F] [--speed F km/h]\n"
        "                 [--track F] [--nsat N] [--fix-valid 0|1]\n"
        "                 [--utc \"YYYY-MM-DDTHH:MM:SSZ\"] [--datum WGS-84] [--force]\n"
        "       hginject --revert <file>\n"
        "formats: 3FR (TIFF), JPG (EXIF APP1 or XMP), HEIC/HIF - sniffed by magic\n"
        "exit codes: 0 ok, 1 skipped, 2 usage, 3 io, 4 format, 5 no space\n");
}

static int parse_utc(const char *s, hg_opt *o)
{
    if (strlen(s) != 20 || s[4] != '-' || s[7] != '-' ||
        (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':' || s[19] != 'Z')
        return 0;
    static const int pos[6] = { 0, 5, 8, 11, 14, 17 };
    static const int wid[6] = { 4, 2, 2, 2, 2, 2 };
    int v[6];
    for (int i = 0; i < 6; i++) {
        int x = 0;
        for (int k = 0; k < wid[i]; k++) {
            if (s[pos[i] + k] < '0' || s[pos[i] + k] > '9') return 0;
            x = x * 10 + (s[pos[i] + k] - '0');
        }
        v[i] = x;
    }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 ||
        v[3] > 23 || v[4] > 59 || v[5] > 60) return 0;
    o->yy = v[0]; o->mo = v[1]; o->dd = v[2];
    o->hh = v[3]; o->mi = v[4]; o->ss = v[5];
    o->has_utc = 1;
    return 1;
}

enum { FMT_3FR, FMT_JPG, FMT_HEIC, FMT_BAD };

static int sniff(int fd)
{
    u8 m[12];
    if (pread_full(fd, m, sizeof m, 0) != HG_OK) return FMT_BAD;
    if (m[0] == 0xFF && m[1] == 0xD8) return FMT_JPG;
    if (!memcmp(m + 4, "ftyp", 4)) return FMT_HEIC;
    if (tiff_magic_le(m, sizeof m) >= 0) return FMT_3FR;
    return FMT_BAD;
}

static int parse_opt_val(const char *a, int pre, int argc, char **argv, int *i, const char **v)
{
    /* supports "--name=value" and "--name value"; a[pre] is 0 or '='.
     * a following token counts as a value unless it looks like an option:
     * "--anything" or exactly "-"; a negative number ("-33.8568") is a value */
    if (a[pre] == '=') { *v = a + pre + 1; return 1; }
    if (a[pre] == 0 && *i + 1 < argc) {
        const char *n = argv[*i + 1];
        int is_opt = (n[0] == '-' && n[1] == '-') || !strcmp(n, "-");
        if (!is_opt) { *v = argv[++*i]; return 1; }
        *v = "";                                      /* missing value */
        return 1;
    }
    if (a[pre] == 0) { *v = ""; return 1; }           /* missing value -> error later */
    return 0;
}

int main(int argc, char **argv)
{
    hg_opt o;
    opt_defaults(&o);
    const char *path = NULL;
    const char *v;
    int revert = 0, force = 0, have_lat = 0, have_lon = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--revert")) revert = 1;
        else if (!strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return HG_OK; }
        else if (!strncmp(a, "--lat", 5) && parse_opt_val(a, 5, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --lat needs a value\n"); return HG_USAGE; }
            o.lat = strtod(v, NULL); have_lat = 1;
            if (o.lat < -90.0 || o.lat > 90.0) { fprintf(stderr, "hginject: --lat out of range\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--lon", 5) && parse_opt_val(a, 5, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --lon needs a value\n"); return HG_USAGE; }
            o.lon = strtod(v, NULL); have_lon = 1;
            if (o.lon < -180.0 || o.lon > 180.0) { fprintf(stderr, "hginject: --lon out of range\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--alt", 5) && parse_opt_val(a, 5, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --alt needs a value\n"); return HG_USAGE; }
            o.alt = strtod(v, NULL); o.has_alt = 1;
        }
        else if (!strncmp(a, "--dop", 5) && parse_opt_val(a, 5, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --dop needs a value\n"); return HG_USAGE; }
            o.dop = strtod(v, NULL); o.has_dop = 1;
            if (o.dop < 0) { fprintf(stderr, "hginject: --dop must be >= 0\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--speed", 7) && parse_opt_val(a, 7, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --speed needs a value\n"); return HG_USAGE; }
            o.speed = strtod(v, NULL); o.has_speed = 1;
            if (o.speed < 0) { fprintf(stderr, "hginject: --speed must be >= 0\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--track", 7) && parse_opt_val(a, 7, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --track needs a value\n"); return HG_USAGE; }
            o.track = strtod(v, NULL); o.has_track = 1;
            if (o.track < 0.0 || o.track >= 360.0) { fprintf(stderr, "hginject: --track must be in [0,360)\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--nsat", 6) && parse_opt_val(a, 6, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --nsat needs a value\n"); return HG_USAGE; }
            char *ep; long nv = strtol(v, &ep, 10);
            if (*ep || nv < 0 || nv > 999) { fprintf(stderr, "hginject: --nsat must be 0..999\n"); return HG_USAGE; }
            o.nsat = nv; o.has_nsat = 1;
        }
        else if (!strncmp(a, "--fix-valid", 11) && parse_opt_val(a, 11, argc, argv, &i, &v)) {
            if (!strcmp(v, "0")) o.fix_valid = 0;
            else if (!strcmp(v, "1")) o.fix_valid = 1;
            else { fprintf(stderr, "hginject: --fix-valid must be 0 or 1\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--utc", 5) && parse_opt_val(a, 5, argc, argv, &i, &v)) {
            if (!parse_utc(v, &o)) { fprintf(stderr, "hginject: --utc must be YYYY-MM-DDTHH:MM:SSZ\n"); return HG_USAGE; }
        }
        else if (!strncmp(a, "--datum", 7) && parse_opt_val(a, 7, argc, argv, &i, &v)) {
            if (!*v) { fprintf(stderr, "hginject: --datum needs a value\n"); return HG_USAGE; }
            if (strlen(v) >= sizeof o.datum) { fprintf(stderr, "hginject: --datum too long\n"); return HG_USAGE; }
            strcpy(o.datum, v);
        }
        else if (a[0] == '-') { fprintf(stderr, "hginject: unknown option %s\n", a); usage(stderr); return HG_USAGE; }
        else if (!path) path = a;
        else { fprintf(stderr, "hginject: multiple files given\n"); usage(stderr); return HG_USAGE; }
    }
    if (!path) { usage(stderr); return HG_USAGE; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "hginject: %s: open: %s\n", path, strerror(errno)); return HG_IO; }
    int fmt = sniff(fd);
    close(fd);
    if (fmt == FMT_BAD) {
        fprintf(stderr, "hginject: %s: unrecognized format (not TIFF/JPEG/HEIF)\n", path);
        return HG_FORMAT;
    }

    if (revert) {
        switch (fmt) {
        case FMT_3FR:  return revert_3fr(path);
        case FMT_JPG:  return revert_jpg(path);
        case FMT_HEIC: return revert_heic(path);
        default: return HG_FORMAT;
        }
    }

    if (!have_lat || !have_lon) {
        fprintf(stderr, "hginject: --lat and --lon are required\n");
        usage(stderr);
        return HG_USAGE;
    }
    if (!o.has_utc) {                                /* default: current UTC */
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        if (!tm) { fprintf(stderr, "hginject: gmtime failed\n"); return HG_IO; }
        o.yy = tm->tm_year + 1900; o.mo = tm->tm_mon + 1; o.dd = tm->tm_mday;
        o.hh = tm->tm_hour; o.mi = tm->tm_min; o.ss = tm->tm_sec;
    }

    if (force) {
        int r;
        switch (fmt) {
        case FMT_3FR:  r = revert_3fr(path); break;
        case FMT_JPG:  r = revert_jpg(path); break;
        case FMT_HEIC: r = revert_heic(path); break;
        default: return HG_FORMAT;
        }
        if (r == HG_OK) printf("hginject: %s: --force: previous injection reverted\n", path);
        else if (r != HG_SKIP) return r;
    }

    switch (fmt) {
    case FMT_3FR:  return inject_3fr(path, &o);
    case FMT_JPG:  return inject_jpg(path, &o);
    case FMT_HEIC: return inject_heic(path, &o);
    default: return HG_FORMAT;
    }
}
