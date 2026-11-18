#ifndef SPLUS_GPS_PUBLICATION_H
#define SPLUS_GPS_PUBLICATION_H

#include "gps_policy.h"
#include "../gps_photo_cache.h"

typedef struct {
    struct gps_photo_cache *wire;
    uint64_t sequence, boot_hi, boot_lo, generation;
} GpsPublication;

bool gps_publication_open(GpsPublication *publication, const char *path, uint64_t now);
void gps_publication_write(GpsPublication *publication, const GpsPolicy *policy, uint64_t now);
void gps_publication_close(GpsPublication *publication);

#endif
