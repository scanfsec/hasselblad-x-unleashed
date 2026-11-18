#include "gps_dbus.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

struct GpsDbus {
    DBusConnection *connection;
    GpsDbusCallbacks callbacks;
    void *context;
};

static const char introspection[] =
    "<node>"
    "<interface name='" GPS_DBUS_NAME "'>"
    "<method name='GetStatus'><arg name='status' type='a{sv}' direction='out'/></method>"
    "<method name='SetEnabled'><arg name='enabled' type='b' direction='in'/>"
    "<arg name='status' type='a{sv}' direction='out'/></method>"
    "</interface>"
    "<interface name='org.freedesktop.DBus.Introspectable'>"
    "<method name='Introspect'><arg name='xml' type='s' direction='out'/></method>"
    "</interface>"
    "</node>";

static bool dict_value(DBusMessageIter *dict, const char *key, int type, const void *value)
{
    const char signature[] = {(char)type, '\0'};
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant)) {
        dbus_message_iter_abandon_container(dict, &entry);
        return false;
    }
    if (!dbus_message_iter_append_basic(&variant, type, value)) {
        dbus_message_iter_abandon_container(&entry, &variant);
        dbus_message_iter_abandon_container(dict, &entry);
        return false;
    }
    if (!dbus_message_iter_close_container(&entry, &variant)) {
        dbus_message_iter_abandon_container(dict, &entry);
        return false;
    }
    return dbus_message_iter_close_container(dict, &entry);
}

static DBusMessage *status_reply(GpsDbus *bus, DBusMessage *request)
{
    GpsDbusStatus state = {.phase = "off", .live_state = "hidden", .pause_reason = "",
                          .photo_write_state = "unavailable", .cache_age_seconds = -1,
                          .source = "none", .position_source = "none",
                          .horizontal_accuracy_m = -1, .vertical_accuracy_m = -1};
    bus->callbacks.get_status(bus->context, &state);
    if (!state.source)
        state.source = "none";
    if (!state.position_source)
        state.position_source = "none";
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (!reply)
        return NULL;
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(reply, &iter);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict)) {
        dbus_message_unref(reply);
        return NULL;
    }
    dbus_bool_t available = state.available, enabled = state.enabled, pending = state.pending;
    dbus_bool_t cache_valid = state.cache_valid;
    dbus_bool_t hotshoe_active = state.hotshoe_active, bluetooth_active = state.bluetooth_active;
    const char *utc = state.position_utc;
    const bool encoded = dict_value(&dict, "available", DBUS_TYPE_BOOLEAN, &available) &&
        dict_value(&dict, "enabled", DBUS_TYPE_BOOLEAN, &enabled) &&
        dict_value(&dict, "pending", DBUS_TYPE_BOOLEAN, &pending) &&
        dict_value(&dict, "phase", DBUS_TYPE_STRING, &state.phase) &&
        dict_value(&dict, "liveState", DBUS_TYPE_STRING, &state.live_state) &&
        dict_value(&dict, "pauseReason", DBUS_TYPE_STRING, &state.pause_reason) &&
        dict_value(&dict, "cacheValid", DBUS_TYPE_BOOLEAN, &cache_valid) &&
        dict_value(&dict, "cacheAgeSeconds", DBUS_TYPE_INT32, &state.cache_age_seconds) &&
        dict_value(&dict, "positionUtc", DBUS_TYPE_STRING, &utc) &&
        dict_value(&dict, "horizontalAccuracyM", DBUS_TYPE_DOUBLE, &state.horizontal_accuracy_m) &&
        dict_value(&dict, "verticalAccuracyM", DBUS_TYPE_DOUBLE, &state.vertical_accuracy_m) &&
        dict_value(&dict, "photoWriteState", DBUS_TYPE_STRING, &state.photo_write_state) &&
        dict_value(&dict, "source", DBUS_TYPE_STRING, &state.source) &&
        dict_value(&dict, "positionSource", DBUS_TYPE_STRING, &state.position_source) &&
        dict_value(&dict, "hotshoeActive", DBUS_TYPE_BOOLEAN, &hotshoe_active) &&
        dict_value(&dict, "bluetoothActive", DBUS_TYPE_BOOLEAN, &bluetooth_active);
    if (!encoded) {
        dbus_message_iter_abandon_container(&iter, &dict);
        dbus_message_unref(reply);
        return NULL;
    }
    if (!dbus_message_iter_close_container(&iter, &dict)) {
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

static bool can_set_enabled(DBusConnection *connection, DBusMessage *request)
{
    const char *sender = dbus_message_get_sender(request);
    if (!sender)
        return false;
    DBusError error = DBUS_ERROR_INIT;
    unsigned long uid = dbus_bus_get_unix_user(connection, sender, &error);
    dbus_error_free(&error);
    return uid == 0 || uid == (unsigned long)geteuid();
}

static DBusHandlerResult handle(DBusConnection *connection, DBusMessage *request, void *context)
{
    if (dbus_message_get_type(request) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    GpsDbus *bus = context;
    DBusMessage *reply;
    if (dbus_message_is_method_call(request, GPS_DBUS_NAME, "GetStatus")) {
        reply = dbus_message_has_signature(request, "") ? status_reply(bus, request) :
            dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS, "GetStatus takes no arguments");
    } else if (dbus_message_is_method_call(request, GPS_DBUS_NAME, "SetEnabled")) {
        dbus_bool_t enabled;
        if (!dbus_message_get_args(request, NULL, DBUS_TYPE_BOOLEAN, &enabled, DBUS_TYPE_INVALID))
            reply = dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS, "SetEnabled takes one boolean");
        else if (!can_set_enabled(connection, request))
            reply = dbus_message_new_error(request, DBUS_ERROR_ACCESS_DENIED, "GPS switch requires the camera service UID");
        else if (!bus->callbacks.set_enabled(bus->context, enabled))
            reply = dbus_message_new_error(request, DBUS_ERROR_FAILED, "Could not persist the GPS switch");
        else
            reply = status_reply(bus, request);
    } else if (dbus_message_is_method_call(request, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        const char *xml = introspection;
        if (!dbus_message_has_signature(request, ""))
            reply = dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS, "Introspect takes no arguments");
        else if ((reply = dbus_message_new_method_return(request)) &&
                 !dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            reply = NULL;
        }
    } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (!reply)
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
    const bool sent = dbus_connection_send(connection, reply, NULL);
    dbus_message_unref(reply);
    return sent ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
}

GpsDbus *gps_dbus_open(const GpsDbusCallbacks *callbacks, void *context)
{
    static const DBusObjectPathVTable handler = {.message_function = handle};
    if (!callbacks || !callbacks->get_status || !callbacks->set_enabled)
        return NULL;
    GpsDbus *bus = calloc(1, sizeof(*bus));
    if (!bus)
        return NULL;
    bus->callbacks = *callbacks;
    bus->context = context;
    DBusError error = DBUS_ERROR_INIT;
    bus->connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    dbus_error_free(&error);
    if (!bus->connection) {
        free(bus);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(bus->connection, false);
    if (!dbus_connection_register_object_path(bus->connection, GPS_DBUS_PATH, &handler, bus) ||
        dbus_bus_request_name(bus->connection, GPS_DBUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) !=
            DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        dbus_error_free(&error);
        gps_dbus_close(bus);
        return NULL;
    }
    return bus;
}

DBusConnection *gps_dbus_connection(GpsDbus *bus)
{
    return bus ? bus->connection : NULL;
}

int gps_dbus_fd(GpsDbus *bus)
{
    int fd;
    return bus && dbus_connection_get_unix_fd(bus->connection, &fd) ? fd : -1;
}

bool gps_dbus_dispatch(GpsDbus *bus, int timeout_ms)
{
    if (!bus)
        return false;
    DBusConnection *connection = bus->connection;
    if (!dbus_connection_read_write(connection, timeout_ms))
        return false;
    for (unsigned i = 0; i < 32; ++i) {
        DBusDispatchStatus status = dbus_connection_dispatch(connection);
        if (status != DBUS_DISPATCH_DATA_REMAINS)
            return status == DBUS_DISPATCH_COMPLETE;
    }
    return true;
}

void gps_dbus_close(GpsDbus *bus)
{
    if (!bus)
        return;
    dbus_connection_close(bus->connection);
    dbus_connection_unref(bus->connection);
    free(bus);
}

void gps_dbus_policy_status(GpsDbusStatus *status, const GpsPolicy *policy, uint64_t now)
{
    const GpsFix *latest = gps_fix_cache_latest(&policy->cache);
    const GpsFix *valid = gps_fix_cache_at(&policy->cache, now);
    uint64_t age = latest && latest->received_ns <= now ? (now - latest->received_ns) / GPS_NS_PER_SECOND : 0;
    *status = (GpsDbusStatus){
        .available = true, .enabled = policy->enabled,
        .pending = policy->release_requested || policy->link == GPS_LINK_STOPPING,
        .cache_valid = valid != NULL,
        .phase = gps_policy_phase(policy, now), .live_state = gps_policy_live(policy, now),
        .pause_reason = gps_policy_pause_reason(policy),
        .photo_write_state = !policy->enabled ? "off" : !valid ? "waitingCache" :
                             gps_policy_photo_allowed(policy, now) ? "ready" : "waitingFeed",
        .cache_age_seconds = !latest || latest->received_ns > now ? -1 : age > INT32_MAX ? INT32_MAX : (int32_t)age,
        .source = gps_source_name(policy->source),
        .position_source = valid ? gps_source_name(policy->position_source) : "none",
        .horizontal_accuracy_m = valid && valid->horizontal_accuracy_mm != GPS_ACCURACY_UNKNOWN ?
                                 valid->horizontal_accuracy_mm / 1000.0 : -1,
        .vertical_accuracy_m = valid && valid->has_altitude &&
                               valid->vertical_accuracy_mm != GPS_ACCURACY_UNKNOWN ?
                               valid->vertical_accuracy_mm / 1000.0 : -1,
    };
    if (valid)
        (void)gps_fix_utc_text(valid, status->position_utc, sizeof(status->position_utc));
}
