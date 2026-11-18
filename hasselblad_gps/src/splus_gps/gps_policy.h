#ifndef SPLUS_GPS_POLICY_H
#define SPLUS_GPS_POLICY_H

#include "gps_core.h"

typedef enum {
    GPS_SOURCE_BLE, GPS_SOURCE_NONE, GPS_SOURCE_HOTSHOE
} GpsSource;

typedef enum {
    GPS_LINK_IDLE, GPS_LINK_DISCOVERING, GPS_LINK_OPENING, GPS_LINK_STREAMING,
    GPS_LINK_STOPPING, GPS_LINK_UNCERTAIN
} GpsLinkState;

typedef struct {
    bool awake, video, mic_known, mic_installed, mic_direct, mic_scan, bridge_available;
} GpsConditions;

typedef enum {
    GPS_ACTION_NONE, GPS_ACTION_OPEN, GPS_ACTION_DISCOVER, GPS_ACTION_CONFIRM, GPS_ACTION_STOP
} GpsAction;

typedef struct {
    GpsFixCache cache;
    GpsStream stream;
    GpsConditions conditions;
    GpsLinkState link;
    GpsSource source, position_source;
    uint64_t enable_epoch, enabled_since_ns, trusted_from_ns;
    uint64_t connected_ns, last_navigation_ns, last_fix_ns, last_receipt_ns;
    uint64_t retry_ns, setup_deadline_ns;
    unsigned retry_seconds;
    bool enabled, discovery_complete, needs_discovery, recovery_scan_used, release_requested;
} GpsPolicy;

void gps_policy_init(GpsPolicy *policy, bool enabled, uint64_t now);
void gps_policy_set_enabled(GpsPolicy *policy, bool enabled, uint64_t now);
void gps_policy_set_conditions(GpsPolicy *policy, GpsConditions conditions);
void gps_policy_set_source(GpsPolicy *policy, GpsSource source, uint64_t now);
const char *gps_source_name(GpsSource source);
GpsAction gps_policy_step(GpsPolicy *policy, uint64_t now);
void gps_policy_discovered(GpsPolicy *policy);
void gps_policy_connected(GpsPolicy *policy, uint64_t now);
void gps_policy_receive(GpsPolicy *policy, const uint8_t *packet, size_t size, uint64_t now);
void gps_policy_receive_hotshoe(GpsPolicy *policy, uint8_t byte, uint64_t now, bool discontinuity);
void gps_policy_stopped(GpsPolicy *policy, unsigned status, uint64_t now);
void gps_policy_bridge_lost(GpsPolicy *policy);
const char *gps_policy_phase(const GpsPolicy *policy, uint64_t now);
const char *gps_policy_live(const GpsPolicy *policy, uint64_t now);
const char *gps_policy_pause_reason(const GpsPolicy *policy);
bool gps_policy_photo_allowed(const GpsPolicy *policy, uint64_t now);

#endif
