#ifndef HBL_GPS_BSA_TRANSPORT_H
#define HBL_GPS_BSA_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define GPS_BSA_SOCKET "/data/hasselblad-gps/run/bsa.sock"
#define GPS_BSA_VERSION 1u
#define GPS_BSA_DATA_MAX 244u
#define GPS_BSA_DISCOVER 1u

enum gps_bsa_message_type {
    GPS_BSA_START = 1,
    GPS_BSA_STOP,
    GPS_BSA_CONNECTED,
    GPS_BSA_DATA,
    GPS_BSA_STOPPED,
    GPS_BSA_FAULT,
    GPS_BSA_DISCOVERED,
    GPS_BSA_CONFIRM,
    GPS_BSA_READY,
};

/* Local SOCK_SEQPACKET ABI on build 343. START/STOP/CONFIRM use the same
 * nonzero sequence for one attempt. Events echo it; READY uses sequence zero
 * and confirms admission before commands. STOPPED proves cleanup;
 * FAULT means this Bluetooth process must not be asked to register again. */
struct gps_bsa_message {
    uint32_t version;
    uint32_t type;
    uint64_t sequence;
    uint64_t boottime_ns;
    uint32_t status;
    uint32_t size;
    uint8_t data[GPS_BSA_DATA_MAX];
    uint8_t reserved[4];
};

typedef void (*gps_bsa_report_fn)(void *context, const struct gps_bsa_message *message);

/* All calls except the native callback run on the bridge's one worker. */
int gps_bsa_install(gps_bsa_report_fn report, void *context);
int gps_bsa_start(uint64_t sequence, const uint8_t peer[6]);
void gps_bsa_stop(void);
int gps_bsa_event_fd(void);
/* Transaction deadlines exclude time spent in system suspend. */
uint64_t gps_bsa_deadline_ns(void);
void gps_bsa_tick(void);
uint64_t gps_bsa_boottime_ns(void);
uint64_t gps_bsa_monotonic_ns(void);

#endif
