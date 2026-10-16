/* nettest - standalone netpad receiver for bring-up debugging.
 * Binds udp 7777, prints every packet (or recv errors) for 15s. */
#include <stdio.h>
#include <errno.h>
#include "netpad.h"

int main(void) {
    if (netpad_init() != 0) { printf("nettest: bind failed errno=%d\n", errno); return 1; }
    printf("nettest: listening on %d\n", NETPAD_PORT);
    for (int i = 0; i < 150; i++) {
        netpad_poll();
        static unsigned prev_btn = 0xffffffff;
        static int prev_ax[4] = {-1,-1,-1,-1};
        unsigned b = netpad_buttons();
        if (b != prev_btn) { printf("btn=%04x alive=%d\n", b, netpad_alive()); prev_btn = b; }
        for (int a = 0; a < 4; a++) {
            int v = netpad_norm(a);
            if (v != prev_ax[a]) { printf("ax%d=%d\n", a, v); prev_ax[a] = v; }
        }
        usleep(100000);
    }
    printf("nettest: done\n");
    return 0;
}
