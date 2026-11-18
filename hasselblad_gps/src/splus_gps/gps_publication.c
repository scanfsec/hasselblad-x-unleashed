#include "gps_publication.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(struct gps_photo_cache) == (11 + GPS_PHOTO_CACHE_FIXES * 9) * 8, "GPS photo cache ABI");
_Static_assert(__atomic_always_lock_free(8, 0), "GPS photo cache requires atomic 64-bit words");

static bool read_boot_id(uint64_t *hi, uint64_t *lo)
{
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!file)
        return false;
    uint32_t a, b, c, d;
    uint64_t e;
    int count = fscanf(file, "%8" SCNx32 "-%4" SCNx32 "-%4" SCNx32 "-%4" SCNx32 "-%12" SCNx64,
                       &a, &b, &c, &d, &e);
    fclose(file);
    if (count != 5)
        return false;
    *hi = (uint64_t)a << 32 | (uint64_t)b << 16 | c;
    *lo = (uint64_t)d << 48 | e;
    return true;
}

bool gps_publication_open(GpsPublication *publication, const char *path, uint64_t now)
{
    *publication = (GpsPublication){.generation = now};
    if (!now || !read_boot_id(&publication->boot_hi, &publication->boot_lo))
        return false;
    char staging[256];
    int size = snprintf(staging, sizeof(staging), "%s.XXXXXX", path);
    if (size < 0 || (size_t)size >= sizeof(staging))
        return false;
    int fd = mkostemp(staging, O_CLOEXEC);
    if (fd < 0)
        return false;
    void *mapping = MAP_FAILED;
    if (fchmod(fd, 0600) == 0 && posix_fallocate(fd, 0, sizeof(struct gps_photo_cache)) == 0)
        mapping = mmap(NULL, sizeof(struct gps_photo_cache), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED || rename(staging, path) < 0) {
        if (mapping != MAP_FAILED)
            munmap(mapping, sizeof(struct gps_photo_cache));
        unlink(staging);
        return false;
    }
    publication->wire = mapping;
    return true;
}

#define STORE(field, value) __atomic_store_n(&(field), (value), __ATOMIC_SEQ_CST)

void gps_publication_write(GpsPublication *publication, const GpsPolicy *policy, uint64_t now)
{
    struct gps_photo_cache *wire = publication->wire;
    bool trusted = gps_policy_photo_allowed(policy, now);
    size_t count = policy->cache.count < GPS_PHOTO_CACHE_FIXES ? policy->cache.count : GPS_PHOTO_CACHE_FIXES;
    STORE(wire->sequence, ++publication->sequence);
    STORE(wire->magic, GPS_PHOTO_CACHE_MAGIC);
    STORE(wire->bytes, sizeof(*wire));
    STORE(wire->boot_hi, publication->boot_hi);
    STORE(wire->boot_lo, publication->boot_lo);
    STORE(wire->writer_generation, publication->generation);
    STORE(wire->enable_epoch, policy->enable_epoch);
    STORE(wire->flags, (policy->enabled ? GPS_PHOTO_CACHE_ENABLED : 0) | (trusted ? GPS_PHOTO_CACHE_TRUSTED : 0));
    STORE(wire->published_ns, now);
    STORE(wire->trusted_from_ns, policy->trusted_from_ns > policy->enabled_since_ns ?
          policy->trusted_from_ns : policy->enabled_since_ns);
    STORE(wire->count, count);
    for (size_t i = 0; i < count; ++i) {
        size_t index = (policy->cache.next + GPS_FIX_HISTORY_CAPACITY - 1 - i) % GPS_FIX_HISTORY_CAPACITY;
        const GpsFix *fix = &policy->cache.entries[index];
        struct gps_photo_cache_fix *out = &wire->fixes[i];
        STORE(out->received_ns, fix->received_ns);
        STORE(out->utc_seconds, fix->utc_seconds);
        STORE(out->latitude_e7, fix->latitude_e7);
        STORE(out->longitude_e7, fix->longitude_e7);
        STORE(out->altitude_mm, fix->altitude_msl_mm);
        STORE(out->utc_nanosecond, fix->utc_nanosecond);
        STORE(out->horizontal_accuracy_mm, fix->horizontal_accuracy_mm);
        STORE(out->vertical_accuracy_mm, fix->vertical_accuracy_mm);
        STORE(out->flags,
              (fix->horizontal_accuracy_mm != GPS_ACCURACY_UNKNOWN ? GPS_PHOTO_FIX_HORIZONTAL_ACCURACY : 0) |
              (fix->has_altitude ? GPS_PHOTO_FIX_ALTITUDE : 0));
    }
    STORE(wire->sequence, ++publication->sequence);
}

void gps_publication_close(GpsPublication *publication)
{
    if (!publication->wire)
        return;
    STORE(publication->wire->sequence, ++publication->sequence);
    STORE(publication->wire->flags, 0);
    STORE(publication->wire->sequence, ++publication->sequence);
    munmap(publication->wire, sizeof(*publication->wire));
    publication->wire = NULL;
}
