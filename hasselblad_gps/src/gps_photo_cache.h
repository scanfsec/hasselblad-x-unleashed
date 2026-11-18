#ifndef HASSELBLAD_GPS_PHOTO_CACHE_H
#define HASSELBLAD_GPS_PHOTO_CACHE_H

#include <stdint.h>

#define GPS_PHOTO_CACHE_PATH "/data/hasselblad-gps/run/photo-cache"
#define GPS_PHOTO_CACHE_MAGIC UINT64_C(0x47505350484f5431)
#define GPS_PHOTO_CACHE_FIXES 32
#define GPS_PHOTO_CACHE_MAX_AGE_NS UINT64_C(3600000000000)
#define GPS_PHOTO_MAILBOX_PATH "/data/pixel-shift/storage/gps-photo-mailbox"
#define GPS_PHOTO_MAILBOX_MAGIC UINT64_C(0x47505350484d4231)

#define GPS_PHOTO_CACHE_ENABLED UINT64_C(1)
#define GPS_PHOTO_CACHE_TRUSTED UINT64_C(2)
#define GPS_PHOTO_FIX_ALTITUDE UINT64_C(1)
#define GPS_PHOTO_FIX_HORIZONTAL_ACCURACY UINT64_C(2)

/* Fixed little-endian ARM64 wire. Every field occupies one aligned, lock-free
 * 64-bit word. Values of signed fields use two's complement representation. */
struct gps_photo_cache_fix {
    uint64_t received_ns;
    int64_t utc_seconds, latitude_e7, longitude_e7, altitude_mm;
    uint64_t utc_nanosecond, horizontal_accuracy_mm, vertical_accuracy_mm, flags;
};

struct gps_photo_cache {
    uint64_t sequence;       /* 0 is unpublished; odd is an update in progress. */
    uint64_t magic, bytes;
    uint64_t boot_hi, boot_lo; /* UUID as 32 hex digits, split into two integers. */
    uint64_t writer_generation, enable_epoch, flags;
    uint64_t published_ns;   /* Monotonic CLOCK_BOOTTIME of the state publication. */
    uint64_t trusted_from_ns; /* Earliest instant covered by this enable/continuity epoch. */
    uint64_t count;
    struct gps_photo_cache_fix fixes[GPS_PHOTO_CACHE_FIXES]; /* Newest first. */
};

/* Read-only photo writer health prefix shared with the daemon. This reports
 * independently qualified storage and capture processes. A fresh capture
 * heartbeat is required before presenting photo write as ready. */
struct gps_photo_mailbox_status {
    uint64_t generation, magic, storage_ready, capture_heartbeat_ns;
    uint64_t storage_pid, capture_pid;
};

/* The writer creates a private regular file, sizes it once and keeps the same
 * inode for this daemon lifetime. Never truncate a mapped inode. All wire
 * stores and loads use __ATOMIC_SEQ_CST, including sequence. Begin with an odd
 * sequence; publish a new even nonzero sequence only after all fields are set.
 * Invalidate enabled/trusted state when the process shuts down cleanly. */

#endif
