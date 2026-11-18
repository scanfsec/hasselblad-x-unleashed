#include "gps_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

static unsigned month_days(unsigned year, unsigned month)
{
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return days[month - 1] + (month == 2 && leap);
}

bool gps_fix_from_pvt(const uint8_t *data, size_t size, uint64_t received_ns, GpsFix *fix)
{
    if (size != 92 || (data[11] & 7) != 7 || data[20] < 2 || data[20] > 4 ||
        !(data[21] & 1) || (data[78] & 1))
        return false;

    unsigned year = data[4] | data[5] << 8;
    unsigned month = data[6], day = data[7];
    int32_t nano = (int32_t)le32(data + 16);
    int32_t lat = (int32_t)le32(data + 28), lon = (int32_t)le32(data + 24);
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || day > month_days(year, month) ||
        data[8] > 23 || data[9] > 59 || data[10] > 59 || nano <= -1000000000 || nano >= 1000000000 ||
        lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000)
        return false;

    struct tm utc = {.tm_year = (int)year - 1900, .tm_mon = (int)month - 1, .tm_mday = (int)day,
                     .tm_hour = data[8], .tm_min = data[9], .tm_sec = data[10]};
    int64_t seconds = timegm(&utc);
    if (nano < 0) {
        --seconds;
        nano += 1000000000;
    }
    *fix = (GpsFix){.received_ns = received_ns, .utc_seconds = seconds, .utc_nanosecond = (uint32_t)nano,
                    .latitude_e7 = lat, .longitude_e7 = lon, .altitude_msl_mm = (int32_t)le32(data + 36),
                    .horizontal_accuracy_mm = le32(data + 40), .vertical_accuracy_mm = le32(data + 44),
                    .has_altitude = data[20] != 2};
    return true;
}

const GpsFix *gps_fix_cache_latest(const GpsFixCache *cache)
{
    return cache->count ? &cache->entries[(cache->next + GPS_FIX_HISTORY_CAPACITY - 1) % GPS_FIX_HISTORY_CAPACITY] : NULL;
}

static int compare_utc(const GpsFix *left, const GpsFix *right)
{
    if (left->utc_seconds != right->utc_seconds)
        return left->utc_seconds < right->utc_seconds ? -1 : 1;
    return (left->utc_nanosecond > right->utc_nanosecond) -
           (left->utc_nanosecond < right->utc_nanosecond);
}

bool gps_fix_cache_add(GpsFixCache *cache, const GpsFix *fix)
{
    const GpsFix *latest = gps_fix_cache_latest(cache);
    int order = latest ? compare_utc(fix, latest) : 1;
    bool altitude_update = latest && order == 0 && !latest->has_altitude && fix->has_altitude &&
                           fix->latitude_e7 == latest->latitude_e7 &&
                           fix->longitude_e7 == latest->longitude_e7 &&
                           fix->received_ns >= latest->received_ns;
    if (latest && (fix->received_ns < latest->received_ns || (!cache->source_changed &&
                  (order < 0 || (order == 0 && !altitude_update)))))
        return false;
    /* Different receivers need not agree on UTC. Begin their photo history
     * only after a valid fix arrives, keeping receipt order across the switch. */
    if (cache->source_changed)
        cache->next = cache->count = 0;
    cache->source_changed = false;
    cache->entries[cache->next] = *fix;
    cache->next = (cache->next + 1) % GPS_FIX_HISTORY_CAPACITY;
    if (cache->count < GPS_FIX_HISTORY_CAPACITY)
        ++cache->count;
    return true;
}

const GpsFix *gps_fix_cache_at(const GpsFixCache *cache, uint64_t exposure_ns)
{
    for (size_t i = 1; i <= cache->count; ++i) {
        const GpsFix *fix = &cache->entries[(cache->next + GPS_FIX_HISTORY_CAPACITY - i) % GPS_FIX_HISTORY_CAPACITY];
        if (fix->received_ns <= exposure_ns)
            return exposure_ns - fix->received_ns < GPS_FIX_TTL_NS ? fix : NULL;
    }
    return NULL;
}

bool gps_fix_utc_text(const GpsFix *fix, char *text, size_t size)
{
    time_t seconds = fix->utc_seconds;
    struct tm utc;
    if (!gmtime_r(&seconds, &utc) || utc.tm_year < -1899 || utc.tm_year > 8099)
        return false;
    int count = snprintf(text, size, "%04d-%02d-%02dT%02d:%02d:%02d.%09uZ", utc.tm_year + 1900,
                         utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, fix->utc_nanosecond);
    return count >= 0 && (size_t)count < size;
}

void gps_stream_discard_partial(GpsStream *stream)
{
    stream->ubx_size = stream->nmea_size = 0;
}

void gps_stream_reset(GpsStream *stream)
{
    gps_stream_discard_partial(stream);
    stream->have_offset = false;
    stream->nmea_rmc_valid = stream->nmea_gga.valid = stream->nmea_void = false;
    stream->nmea_received_ns = stream->nmea_dimension_ns = 0;
    stream->nmea_dimension = 0;
}

static int hex_digit(uint8_t byte)
{
    if (byte >= '0' && byte <= '9')
        return byte - '0';
    byte |= 0x20;
    return byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : -1;
}

static bool navigation_sentence(const uint8_t *data, size_t size)
{
    static const char *talkers[] = {"GP", "GA", "GB", "BD", "GL", "GQ", "GN"};
    static const char *types[] = {"RMC", "VTG", "GGA", "GSA", "GSV", "GLL", "GNS", "ZDA"};
    if (size && data[size - 1] == '\r')
        --size;
    if (size < 10 || data[6] != ',' || data[size - 3] != '*')
        return false;
    int hi = hex_digit(data[size - 2]), lo = hex_digit(data[size - 1]);
    if (hi < 0 || lo < 0)
        return false;
    uint8_t checksum = 0;
    for (size_t i = 1; i < size - 3; ++i)
        checksum ^= data[i];
    if (checksum != (unsigned)(16 * hi + lo))
        return false;
    bool supported = false;
    for (size_t i = 0; i < sizeof(talkers) / sizeof(*talkers); ++i)
        supported |= memcmp(data + 1, talkers[i], 2) == 0;
    if (!supported)
        return false;
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i)
        if (memcmp(data + 3, types[i], 3) == 0)
            return true;
    return false;
}

static bool read_nmea(GpsStream *stream, uint8_t byte)
{
    if (byte == '$') {
        stream->nmea[0] = byte;
        stream->nmea_size = 1;
    } else if (stream->nmea_size && byte == '\n') {
        bool navigation = navigation_sentence(stream->nmea, stream->nmea_size);
        stream->nmea_size = 0;
        return navigation;
    } else if (stream->nmea_size) {
        if (stream->nmea_size == sizeof(stream->nmea) || (byte < 32 && byte != '\r') || byte > 126)
            stream->nmea_size = 0;
        else
            stream->nmea[stream->nmea_size++] = byte;
    }
    return false;
}

static bool decimal_number(const char *text, double *number)
{
    const char *cursor = text;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    const char *digits = cursor;
    while (*cursor >= '0' && *cursor <= '9')
        ++cursor;
    if (cursor == digits)
        return false;
    if (*cursor == '.') {
        ++cursor;
        digits = cursor;
        while (*cursor >= '0' && *cursor <= '9')
            ++cursor;
        if (cursor == digits)
            return false;
    }
    if (*cursor)
        return false;
    *number = strtod(text, NULL);
    return true;
}

static bool nmea_coordinate(const char *text, const char *hemisphere, unsigned degree_digits,
                             int32_t *coordinate)
{
    size_t length = strlen(text);
    if (length < degree_digits + 2 || !hemisphere[0] || hemisphere[1] ||
        text[degree_digits] < '0' || text[degree_digits] > '9' ||
        text[degree_digits + 1] < '0' || text[degree_digits + 1] > '9' ||
        !((degree_digits == 2 && (hemisphere[0] == 'N' || hemisphere[0] == 'S')) ||
          (degree_digits == 3 && (hemisphere[0] == 'E' || hemisphere[0] == 'W'))))
        return false;
    unsigned degrees = 0;
    for (unsigned i = 0; i < degree_digits; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        degrees = degrees * 10 + (unsigned)(text[i] - '0');
    }
    double minutes;
    unsigned maximum = degree_digits == 2 ? 90 : 180;
    if (!decimal_number(text + degree_digits, &minutes) || minutes < 0 || minutes >= 60 ||
        degrees > maximum || (degrees == maximum && minutes != 0))
        return false;
    double scaled = ((double)degrees + minutes / 60) * 10000000;
    *coordinate = (int32_t)(scaled + 0.5);
    if (hemisphere[0] == 'S' || hemisphere[0] == 'W')
        *coordinate = -*coordinate;
    return true;
}

static bool two_digits(const char *text, unsigned *number)
{
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9')
        return false;
    *number = (unsigned)(text[0] - '0') * 10 + (unsigned)(text[1] - '0');
    return true;
}

static bool nmea_time(const char *text, uint32_t *seconds, uint32_t *nanosecond)
{
    unsigned hour, minute, second;
    if (strlen(text) < 6 || !two_digits(text, &hour) || !two_digits(text + 2, &minute) ||
        !two_digits(text + 4, &second) || hour > 23 || minute > 59 || second > 59)
        return false;
    *seconds = hour * 3600 + minute * 60 + second;
    *nanosecond = 0;
    if (!text[6])
        return true;
    if (text[6] != '.' || !text[7])
        return false;
    unsigned digits = 0;
    for (const char *cursor = text + 7; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9' || digits == 9)
            return false;
        *nanosecond = *nanosecond * 10 + (unsigned)(*cursor - '0');
        ++digits;
    }
    while (digits++ < 9)
        *nanosecond *= 10;
    return true;
}

static bool nmea_date(const char *text, uint32_t time_of_day, int64_t *seconds)
{
    unsigned day, month, year;
    if (strlen(text) != 6 || !two_digits(text, &day) || !two_digits(text + 2, &month) ||
        !two_digits(text + 4, &year))
        return false;
    year += year < 80 ? 2000 : 1900;
    if (month < 1 || month > 12 || day < 1 || day > month_days(year, month))
        return false;
    struct tm utc = {.tm_year = (int)year - 1900, .tm_mon = (int)month - 1,
                     .tm_mday = (int)day};
    *seconds = (int64_t)timegm(&utc) + time_of_day;
    return true;
}

static bool recent_receipt(uint64_t stamp, uint64_t now)
{
    return stamp <= now && now - stamp < GPS_FRAME_TIMEOUT_NS;
}

static bool satellite_mode(const char *mode)
{
    return !mode || !*mode || (!mode[1] && strchr("ADPRF", mode[0]));
}

/* A time of day refers to the nearest civil day, including across midnight. */
static int64_t day_difference(int64_t delta_ns)
{
    const int64_t day = INT64_C(86400) * 1000000000;
    if (delta_ns > day / 2)
        return delta_ns - day;
    if (delta_ns < -day / 2)
        return delta_ns + day;
    return delta_ns;
}

static bool date_position(const GpsStream *stream, const GpsFixCache *cache,
                          uint32_t time_of_day, GpsFix *fix)
{
    if (!stream->nmea_rmc_valid)
        return false;
    const GpsFix *anchor = &stream->nmea_rmc;
    const GpsFix *latest = gps_fix_cache_latest(cache);
    if (latest && !cache->source_changed && latest->received_ns > anchor->received_ns)
        anchor = latest;
    if (fix->received_ns < anchor->received_ns)
        return false;
    uint64_t elapsed = fix->received_ns - anchor->received_ns;
    uint64_t fraction = anchor->utc_nanosecond + elapsed % GPS_NS_PER_SECOND;
    int64_t expected = anchor->utc_seconds + (int64_t)(elapsed / GPS_NS_PER_SECOND) +
                       (int64_t)(fraction / GPS_NS_PER_SECOND);
    int64_t delta = day_difference(((int64_t)time_of_day - expected % 86400) * INT64_C(1000000000));
    fix->utc_seconds = expected + delta / INT64_C(1000000000);
    delta += fix->utc_nanosecond - (int64_t)(fraction % GPS_NS_PER_SECOND);
    /* Limit clock disagreement, not the age of the last dated RMC. */
    return llabs(delta) < (int64_t)GPS_FRAME_TIMEOUT_NS;
}

static bool rejected_epoch(const GpsStream *stream, uint64_t tod_ns, uint64_t now)
{
    return stream->nmea_void && recent_receipt(stream->nmea_void_received_ns, now) &&
           day_difference((int64_t)tod_ns - (int64_t)stream->nmea_void_time_ns) <= 0;
}

static void publish_nmea(GpsStream *stream, GpsFixCache *cache, GpsFix *fix,
                         uint64_t now, GpsStreamResult *result)
{
    const GpsNmeaGga *gga = &stream->nmea_gga;
    uint32_t tod = (uint32_t)(fix->utc_seconds % 86400);
    if (rejected_epoch(stream, tod * GPS_NS_PER_SECOND + fix->utc_nanosecond, now))
        return;
    const GpsFix *latest = gps_fix_cache_latest(cache);
    bool same_epoch = latest && !cache->source_changed && compare_utc(fix, latest) == 0;
    if (same_epoch && !recent_receipt(latest->received_ns, now))
        return;
    bool two_dimensional = stream->nmea_dimension && stream->nmea_dimension < 3 &&
                           recent_receipt(stream->nmea_dimension_ns, now);
    if (gga->valid && recent_receipt(gga->received_ns, now) &&
        tod == gga->seconds_of_day && fix->utc_nanosecond == gga->nanosecond &&
        fix->latitude_e7 == gga->latitude_e7 && fix->longitude_e7 == gga->longitude_e7 &&
        gga->has_altitude && !two_dimensional) {
        fix->has_altitude = true;
        fix->altitude_msl_mm = gga->altitude_msl_mm;
    }
    if (gps_fix_cache_add(cache, fix)) {
        result->fresh_fix = true;
        result->new_nmea_position |= !same_epoch;
    }
}

static void nmea_fix_sentence(GpsStream *stream, GpsFixCache *cache, GpsStreamResult *result,
                               size_t size, uint64_t now)
{
    char line[129];
    memcpy(line, stream->nmea, size);
    line[size] = '\0';
    char *star = strchr(line, '*');
    if (!star)
        return;
    *star = '\0';
    char *fields[20] = {line};
    size_t count = 1;
    for (char *cursor = line; *cursor; ++cursor) {
        if (*cursor == ',') {
            if (count == sizeof(fields) / sizeof(*fields))
                return;
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    const char *kind = fields[0] + 3;
    GpsFix fix = {.received_ns = now, .horizontal_accuracy_mm = GPS_ACCURACY_UNKNOWN,
                  .vertical_accuracy_mm = GPS_ACCURACY_UNKNOWN};
    if (!strcmp(kind, "GSA")) {
        if ((count != 18 && count != 19) || fields[2][0] < '1' || fields[2][0] > '3' || fields[2][1])
            return;
        stream->nmea_dimension = (uint8_t)(fields[2][0] - '0');
        stream->nmea_dimension_ns = now;
        /* GSA has no epoch or coordinates. Only enrich an already published
         * position matched by this stream's recent GGA. */
        const GpsFix *latest = gps_fix_cache_latest(cache);
        if (!stream->nmea_rmc_valid || !latest || cache->source_changed ||
            latest->has_altitude || !recent_receipt(latest->received_ns, now))
            return;
        fix = *latest;
        fix.received_ns = now;
        publish_nmea(stream, cache, &fix, now, result);
        return;
    }

    bool rmc = !strcmp(kind, "RMC"), gga = !strcmp(kind, "GGA"), gll = !strcmp(kind, "GLL");
    if ((!rmc && !gga && !gll) || (rmc && count < 10) || (gga && count < 15) ||
        (gll && count < 7))
        return;
    uint32_t time_of_day;
    if (!nmea_time(fields[gll ? 5 : 1], &time_of_day, &fix.utc_nanosecond))
        return;
    uint64_t tod_ns = time_of_day * GPS_NS_PER_SECOND + fix.utc_nanosecond;
    const char *status = fields[rmc ? 2 : 6];
    const char *mode = rmc ? fields[12] : fields[7];
    bool valid = gga ? status[0] >= '1' && status[0] <= '5' && !status[1] :
                 !strcmp(status, "A") && satellite_mode(mode);
    if (!valid) {
        bool explicit_void = gga ? status[0] && !status[1] && strchr("0678", status[0]) :
                             !strcmp(status, "V") || (mode && *mode && !mode[1] && strchr("EMSN", *mode));
        if (explicit_void && (tod_ns == stream->nmea_void_time_ns || !rejected_epoch(stream, tod_ns, now))) {
            stream->nmea_void = true;
            stream->nmea_void_time_ns = tod_ns;
            stream->nmea_void_received_ns = now;
        }
        return;
    }
    unsigned latitude = rmc ? 3 : gga ? 2 : 1;
    if (rejected_epoch(stream, tod_ns, now) ||
        !nmea_coordinate(fields[latitude], fields[latitude + 1], 2, &fix.latitude_e7) ||
        !nmea_coordinate(fields[latitude + 2], fields[latitude + 3], 3, &fix.longitude_e7))
        return;
    if (rmc) {
        if (!nmea_date(fields[9], time_of_day, &fix.utc_seconds))
            return;
        const GpsFix *latest = gps_fix_cache_latest(cache);
        if (latest && !cache->source_changed) {
            int order = compare_utc(&fix, latest);
            if (order < 0 || (order == 0 && (fix.latitude_e7 != latest->latitude_e7 ||
                                             fix.longitude_e7 != latest->longitude_e7)))
                return;
        }
        if (!stream->nmea_rmc_valid || compare_utc(&fix, &stream->nmea_rmc) > 0) {
            stream->nmea_rmc = fix;
            stream->nmea_rmc_valid = true;
        }
    } else {
        if (gga) {
            GpsNmeaGga position = {.received_ns = now, .seconds_of_day = time_of_day,
                .nanosecond = fix.utc_nanosecond, .latitude_e7 = fix.latitude_e7,
                .longitude_e7 = fix.longitude_e7, .valid = true};
            double altitude;
            if (!strcmp(fields[10], "M") && decimal_number(fields[9], &altitude) &&
                altitude >= (double)INT32_MIN / 1000 && altitude <= (double)INT32_MAX / 1000) {
                position.altitude_msl_mm = (int32_t)(altitude * 1000 + (altitude < 0 ? -0.5 : 0.5));
                position.has_altitude = true;
            }
            stream->nmea_gga = position;
        }
        if (!date_position(stream, cache, time_of_day, &fix))
            return;
    }
    publish_nmea(stream, cache, &fix, now, result);
}

GpsStreamResult gps_stream_receive_nmea(GpsStream *stream, GpsFixCache *cache,
                                        const uint8_t *data, size_t size, uint64_t received_ns)
{
    GpsStreamResult result = {0};
    if (received_ns < stream->nmea_received_ns) {
        result.malformed = true;
        return result;
    }
    if (stream->nmea_received_ns && received_ns - stream->nmea_received_ns >= GPS_FRAME_TIMEOUT_NS)
        gps_stream_reset(stream);
    stream->nmea_received_ns = received_ns;
    for (size_t i = 0; i < size; ++i) {
        size_t line_size = stream->nmea_size;
        if (read_nmea(stream, data[i])) {
            result.navigation = true;
            nmea_fix_sentence(stream, cache, &result, line_size, received_ns);
        }
    }
    return result;
}

static void discard_ubx(GpsStream *stream, size_t size)
{
    stream->ubx_size -= size;
    memmove(stream->ubx, stream->ubx + size, stream->ubx_size);
}

static void read_ubx(GpsStream *stream, GpsFixCache *cache, GpsStreamResult *result,
                     uint8_t byte, uint64_t received_ns)
{
    uint8_t *frame = stream->ubx;
    stream->ubx[stream->ubx_size++] = byte;
    while (stream->ubx_size) {
        size_t start = 0;
        while (start < stream->ubx_size && (frame[start] != 0xb5 ||
               (start + 1 < stream->ubx_size && frame[start + 1] != 0x62)))
            ++start;
        if (start)
            discard_ubx(stream, start);
        if (stream->ubx_size < 6)
            return;
        size_t payload_size = frame[4] | frame[5] << 8;
        size_t end = payload_size + 8;
        if (end > sizeof(stream->ubx)) {
            discard_ubx(stream, 1);
            continue;
        }
        if (stream->ubx_size < end)
            return;
        uint8_t a = 0, b = 0;
        for (size_t i = 2; i < end - 2; ++i) {
            a += frame[i];
            b += a;
        }
        if (frame[end - 2] != a || frame[end - 1] != b) {
            discard_ubx(stream, 1);
            continue;
        }
        if (frame[2] == 1) {
            result->navigation = true;
            if (frame[3] == 7 && payload_size == 92) {
                GpsFix fix;
                result->pvt = true;
                if (gps_fix_from_pvt(frame + 6, payload_size, received_ns, &fix))
                    result->fresh_fix |= gps_fix_cache_add(cache, &fix);
            }
        }
        discard_ubx(stream, end);
    }
}

GpsStreamResult gps_stream_receive(GpsStream *stream, GpsFixCache *cache, const uint8_t *packet,
                                   size_t size, uint64_t received_ns)
{
    GpsStreamResult result = {0};
    if (size <= 4 || size > 4 + GPS_BLE_RAW_PAYLOAD_MAX) {
        result.malformed = true;
        return result;
    }
    uint32_t offset = le32(packet);
    if (stream->have_offset) {
        uint32_t gap = offset - stream->next_offset;
        if (gap >= UINT32_C(0x80000000)) {
            result.malformed = true;
            return result;
        }
        if (gap) {
            gps_stream_reset(stream);
            result.gap = true;
        }
    }
    stream->have_offset = true;
    stream->next_offset = offset + (uint32_t)(size - 4);
    for (size_t i = 4; i < size; ++i) {
        result.navigation |= read_nmea(stream, packet[i]);
        read_ubx(stream, cache, &result, packet[i], received_ns);
    }
    return result;
}
