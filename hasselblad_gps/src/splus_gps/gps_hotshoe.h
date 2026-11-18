#ifndef SPLUS_GPS_HOTSHOE_H
#define SPLUS_GPS_HOTSHOE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct GpsHotshoe GpsHotshoe;

typedef struct {
    uint64_t bytes, last_byte_ns, discontinuities, overflows, clock_resets;
    int io_error, scheduling_error;
} GpsHotshoeStats;

/* A discontinuity resets the caller's partial parser. Its byte is ignored. */
typedef void (*GpsHotshoeByteFn)(void *context, uint8_t byte,
                                  uint64_t received_boottime_ns, bool discontinuity);

/* GPIO122 belongs to the verified Eagle2 build343 GPIO3 controller, line26. */
GpsHotshoe *gps_hotshoe_open(void);
void gps_hotshoe_close(GpsHotshoe *receiver);
int gps_hotshoe_fd(const GpsHotshoe *receiver);
int gps_hotshoe_drain(GpsHotshoe *receiver, GpsHotshoeByteFn receive, void *context);
GpsHotshoeStats gps_hotshoe_stats(GpsHotshoe *receiver);

#endif
