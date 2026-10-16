/* DJI Mic 2 SPP -> LC3plus 5ms/48k/mono decoder harness (ETSI TS 103 634 ref code).
 * Input : frame dump [u8 len][frame bytes]*  (same format as D:/dji/decode/frames.lc3)
 * Output: 48kHz/16bit/mono WAV
 * Build : NDK clang aarch64-linux-android28, static (bionic)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lc3plus.h"

/* Force PT_TLS p_align=64: static bionic on arm64 requires >=64 or aborts
 * ("TLS segment is underaligned") at startup (NDK r23+). Raw .tbss blob
 * because a C-level aligned TLS var can still be dropped/emulated. */
__asm__(
    ".section .tbss,\"awT\",@nobits\n"
    ".balign 64\n"
    ".globl __tls_align64_pad\n"
    "__tls_align64_pad: .zero 64\n"
    ".size __tls_align64_pad, 64\n"
    ".text\n");

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <frames.lc3> <out.wav>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(fsz);
    if (!buf || fread(buf, 1, fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    /* Decoder config = DJI Mic 2: 48000 Hz, mono, 5.0 ms, hrmode 0, 111 B/frame */
    int need = lc3plus_dec_get_size(48000, 1);
    LC3PLUS_Dec *dec = calloc(1, LC3PLUS_DEC_MAX_SIZE);
    LC3PLUS_Error err = lc3plus_dec_init(dec, 48000, 1, LC3PLUS_PLC_ADVANCED, 0);
    if (err) { fprintf(stderr, "dec_init err=%d\n", err); return 2; }
    err = lc3plus_dec_set_frame_dms(dec, LC3PLUS_FRAME_DURATION_5MS);
    if (err) { fprintf(stderr, "set_frame_dms err=%d\n", err); return 2; }
    int ns = lc3plus_dec_get_output_samples(dec);
    printf("dec_size=%d out_samples/frame=%d (expect 240)\n", need, ns);
    if (ns != 240) { fprintf(stderr, "unexpected frame samples\n"); return 2; }

    int16_t *pcm = NULL; size_t plen = 0, pcap = 0;
    long nfr = 0, nerr = 0, nok = 0;
    int errhist[32]; memset(errhist, 0, sizeof(errhist));
    long pos = 0;
    int16_t fp[LC3PLUS_MAX_SAMPLES];
    uint8_t frame[LC3PLUS_MAX_BYTES];  /* dec16 may write to input -> mutable copy */
    int16_t *outp[1] = { fp };
    while (pos < fsz) {
        int len = buf[pos++];
        if (len <= 0 || pos + len > fsz) break;
        memcpy(frame, buf + pos, len); pos += len;
        memset(fp, 0, sizeof(fp));
        err = lc3plus_dec16(dec, frame, len, outp, NULL, 0);
        nfr++;
        if (err == LC3PLUS_OK) nok++; else { nerr++; if (err>=0 && err<32) errhist[err]++; }
        if (plen + ns > pcap) { pcap = (plen + ns) * 2 + 48000; pcm = realloc(pcm, pcap * 2); }
        memcpy(pcm + plen, fp, ns * 2); plen += ns;
    }
    printf("frames=%ld ok=%ld err=%ld samples=%zu (%.2fs)\n", nfr, nok, nerr, plen, plen / 48000.0);
    for (int i = 0; i < 32; i++) if (errhist[i]) printf("  errcode %d: %ld\n", i, errhist[i]);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 1; }
    uint32_t db = (uint32_t)plen * 2, riff = 36 + db;
    uint32_t fs = 16, sr = 48000, br = 96000; uint16_t ch = 1, fmt = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, o); fwrite(&riff, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o);
    fwrite(&fs, 4, 1, o); fwrite(&fmt, 2, 1, o); fwrite(&ch, 2, 1, o);
    fwrite(&sr, 4, 1, o); fwrite(&br, 4, 1, o); fwrite(&ba, 2, 1, o); fwrite(&bits, 2, 1, o);
    fwrite("data", 1, 4, o); fwrite(&db, 4, 1, o);
    fwrite(pcm, 2, plen, o);
    fclose(o);
    printf("wrote %s\n", argv[2]);
    return 0;
}
