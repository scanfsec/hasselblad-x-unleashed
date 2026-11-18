#include "bsa_bridge.h"
#include "bsa_transport.h"

#include <pthread.h>

static void *start_bridge(void *unused)
{
    (void)unused;
    gps_bsa_bridge_start(GPS_BSA_SOCKET, 0);
    return NULL;
}

/* Android runs constructors under the dynamic loader lock. Socket setup and
 * registration remain outside the constructor and on their own worker. */
__attribute__((constructor)) static void load_bridge(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, start_bridge, NULL))
        pthread_detach(thread);
}
