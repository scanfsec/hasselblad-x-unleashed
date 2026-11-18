#ifndef SPLUS_GPS_DBUS_H
#define SPLUS_GPS_DBUS_H

#include <dbus/dbus.h>

#include "gps_policy.h"

#define GPS_DBUS_NAME "com.hasselblad.splus.Gps"
#define GPS_DBUS_PATH "/com/hasselblad/splus/Gps"

typedef struct GpsDbus GpsDbus;

typedef struct {
    bool available, enabled, pending, cache_valid;
    bool hotshoe_active, bluetooth_active;
    int32_t cache_age_seconds;
    const char *phase, *live_state, *pause_reason, *photo_write_state;
    const char *source, *position_source;
    char position_utc[40];
    double horizontal_accuracy_m, vertical_accuracy_m;
} GpsDbusStatus;

typedef struct {
    void (*get_status)(void *context, GpsDbusStatus *status);
    bool (*set_enabled)(void *context, bool enabled);
} GpsDbusCallbacks;

GpsDbus *gps_dbus_open(const GpsDbusCallbacks *callbacks, void *context);
DBusConnection *gps_dbus_connection(GpsDbus *bus);
int gps_dbus_fd(GpsDbus *bus);
bool gps_dbus_dispatch(GpsDbus *bus, int timeout_ms);
void gps_dbus_close(GpsDbus *bus);

void gps_dbus_policy_status(GpsDbusStatus *status, const GpsPolicy *policy, uint64_t now);

#endif
