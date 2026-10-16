/* netpad - DualSense-over-UDP input source (PC bridge).
 * PC runs pc_padsend.py reading the pad via SDL and streams 16-byte
 * packets to <camera>:7777; games merge the latest packet into their
 * pad scan. Zero-config, stateless, drop-tolerant. */
#ifndef NETPAD_H
#define NETPAD_H

#define NETPAD_PORT   7777
#define NETPAD_MAGIC  0x31445053u  /* "SPD1" */

/* btn bit assignments - must match pc_padsend.py */
#define NP_CROSS     0
#define NP_CIRCLE    1
#define NP_SQUARE    2
#define NP_TRIANGLE  3
#define NP_L1        4
#define NP_R1        5
#define NP_L2        6   /* digital: analog trigger > 1/3 */
#define NP_R2        7
#define NP_CREATE    8
#define NP_OPTIONS   9
#define NP_L3        10
#define NP_R3        11
#define NP_PS        12
#define NP_TOUCHPAD  13
#define NP_UP        14
#define NP_DOWN      15

#ifdef __cplusplus
extern "C" {
#endif

int      netpad_init(void);          /* bind UDP listener; 0 = ok */
void     netpad_poll(void);          /* drain socket, keep latest packet */
unsigned netpad_buttons(void);       /* raw NP_* bitfield */
int      netpad_norm(int axis);      /* axis 0=lx 1=ly 2=rx 3=ry, -512..511 */
int      netpad_alive(void);         /* packet received in the last 500 ms */

#ifdef __cplusplus
}
#endif
#endif
