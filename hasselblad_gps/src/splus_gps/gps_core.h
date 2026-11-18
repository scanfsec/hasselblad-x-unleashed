#ifndef SPLUS_GPS_CORE_H
#define SPLUS_GPS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_NS_PER_SECOND UINT64_C(1000000000)
#define GPS_FIX_TTL_NS (UINT64_C(3600) * GPS_NS_PER_SECOND)
#define GPS_FIRST_FRAME_TIMEOUT_NS (UINT64_C(5) * GPS_NS_PER_SECOND)
#define GPS_FRAME_TIMEOUT_NS (UINT64_C(2500) * UINT64_C(1000000))
#define GPS_FIX_HISTORY_CAPACITY 64
#define GPS_UBX_FRAME_CAPACITY 4104
#define GPS_BLE_RAW_PAYLOAD_MAX 240
#define GPS_ACCURACY_UNKNOWN UINT32_MAX

typedef struct {
    uint64_t received_ns;
    int64_t utc_seconds;
    uint32_t utc_nanosecond;
    int32_t latitude_e7, longitude_e7, altitude_msl_mm;
    uint32_t horizontal_accuracy_mm, vertical_accuracy_mm;
    bool has_altitude;
} GpsFix;

typedef struct {
    GpsFix entries[GPS_FIX_HISTORY_CAPACITY];
    size_t next, count;
    /* A different source may reset UTC ordering. Retain display history until
     * its first accepted fix, while receipt time remains monotonic. */
    bool source_changed;
} GpsFixCache;

typedef struct {
    uint64_t received_ns;
    uint32_t seconds_of_day, nanosecond;
    int32_t latitude_e7, longitude_e7, altitude_msl_mm;
    bool valid, has_altitude;
} GpsNmeaGga;

typedef struct {
    uint8_t ubx[GPS_UBX_FRAME_CAPACITY];
    uint8_t nmea[128];
    size_t ubx_size, nmea_size;
    uint32_t next_offset;
    bool have_offset;
    GpsFix nmea_rmc;
    GpsNmeaGga nmea_gga;
    uint64_t nmea_received_ns, nmea_dimension_ns;
    uint64_t nmea_void_time_ns, nmea_void_received_ns;
    uint8_t nmea_dimension;
    bool nmea_rmc_valid, nmea_void;
} GpsStream;

typedef struct {
    bool gap, malformed, navigation, pvt, fresh_fix;
    /* An accepted NMEA epoch; fresh_fix also includes later altitude enrichment. */
    bool new_nmea_position;
} GpsStreamResult;

bool gps_fix_from_pvt(const uint8_t *data, size_t size, uint64_t received_ns, GpsFix *fix);
bool gps_fix_cache_add(GpsFixCache *cache, const GpsFix *fix);
const GpsFix *gps_fix_cache_latest(const GpsFixCache *cache);
const GpsFix *gps_fix_cache_at(const GpsFixCache *cache, uint64_t exposure_ns);
bool gps_fix_utc_text(const GpsFix *fix, char *text, size_t size);
/* Discard an interrupted wire frame while retaining fresh NMEA epoch fields. */
void gps_stream_discard_partial(GpsStream *stream);
void gps_stream_reset(GpsStream *stream);
GpsStreamResult gps_stream_receive(GpsStream *stream, GpsFixCache *cache, const uint8_t *packet,
                                   size_t size, uint64_t received_ns);
GpsStreamResult gps_stream_receive_nmea(GpsStream *stream, GpsFixCache *cache,
                                        const uint8_t *data, size_t size, uint64_t received_ns);

#endif
