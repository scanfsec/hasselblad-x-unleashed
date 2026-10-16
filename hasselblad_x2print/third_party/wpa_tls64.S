/* wpa_tls64.S — bionic arm64 静态二进制 PT_TLS 对齐垫片
 *
 * 背景: bionic(arm64)动态链接器/静态启动代码要求可执行文件 PT_TLS 段对齐 >=64
 * (TCB/SVE 需求), 否则运行即 abort:
 *   "executable's TLS segment is underaligned: alignment is 8, needs to be
 *    at least 64 for ARM64 Bionic"
 * lld 曾在 D53906 主动放宽过 ARM/AArch64 PT_TLS 对齐, 后在 D62055 回退, 因此
 * NDK r25 的 lld-14 与上游 lld-18 都不会自动放宽(静态 bionic libc.a 自带 align=8
 * 的 .tdata/.tbss, 必然触发)。解法: 链入一个 64 字节对齐的哑 .tdata 节, lld 取
 * 输入 TLS 节最大对齐 => PT_TLS p_align=64 且 vaddr 64 对齐(qemu-aarch64 实测
 * 通过, 见 NOTES §14)。
 */
    .section .tdata,"awT",@progbits
    .balign 64
    .globl __wpa_tls_align64_dummy
    .type __wpa_tls_align64_dummy, %object
__wpa_tls_align64_dummy:
    .quad 0
    .size __wpa_tls_align64_dummy, 8

    .text
    .globl __wpa_tls64_marker
    .type __wpa_tls64_marker, %function
__wpa_tls64_marker:
    ret
