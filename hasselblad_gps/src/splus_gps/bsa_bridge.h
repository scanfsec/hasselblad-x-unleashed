#ifndef HBL_GPS_BSA_BRIDGE_H
#define HBL_GPS_BSA_BRIDGE_H

#include <sys/types.h>

/* Start the one process-lifetime bridge after the installer has verified both
 * Bluetooth images. The path's parent is a private, pre-created directory. */
int gps_bsa_bridge_start(const char *socket_path, uid_t daemon_uid);

#endif
