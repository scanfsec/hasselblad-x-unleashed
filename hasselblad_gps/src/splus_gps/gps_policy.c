#include "gps_policy.h"

#define GPS_SETUP_TIMEOUT_NS (UINT64_C(30) * GPS_NS_PER_SECOND)

static bool direct_allowed(const GpsPolicy *policy)
{
    const GpsConditions *c = &policy->conditions;
    return policy->source == GPS_SOURCE_BLE && policy->enabled && c->awake && !c->video &&
           c->mic_known && c->mic_direct && c->bridge_available;
}

static bool fresh(uint64_t stamp, uint64_t now, uint64_t lifetime)
{
    return stamp && stamp <= now && now - stamp < lifetime;
}

void gps_policy_init(GpsPolicy *policy, bool enabled, uint64_t now)
{
    *policy = (GpsPolicy){.retry_seconds = 2, .source = GPS_SOURCE_BLE};
    gps_policy_set_enabled(policy, enabled, now);
}

void gps_policy_set_enabled(GpsPolicy *policy, bool enabled, uint64_t now)
{
    if (policy->enabled == enabled)
        return;
    policy->enabled = enabled;
    if (!enabled && policy->link != GPS_LINK_IDLE)
        policy->release_requested = true;
    ++policy->enable_epoch;
    policy->enabled_since_ns = enabled ? now : 0;
    policy->retry_ns = 0;
    policy->retry_seconds = 2;
}

void gps_policy_set_conditions(GpsPolicy *policy, GpsConditions conditions)
{
    policy->conditions = conditions;
}

void gps_policy_set_source(GpsPolicy *policy, GpsSource source, uint64_t now)
{
    if (policy->source == source)
        return;
    policy->source = source;
    policy->cache.source_changed = source != GPS_SOURCE_NONE && source != policy->position_source;
    policy->last_navigation_ns = policy->last_fix_ns = policy->last_receipt_ns = 0;
    policy->connected_ns = now;
    gps_stream_reset(&policy->stream);
}

const char *gps_source_name(GpsSource source)
{
    switch (source) {
    case GPS_SOURCE_BLE: return "ble";
    case GPS_SOURCE_HOTSHOE: return "hotshoe";
    default: return "none";
    }
}

static GpsAction request_stop(GpsPolicy *policy)
{
    policy->link = GPS_LINK_STOPPING;
    policy->last_navigation_ns = policy->last_fix_ns = 0;
    return GPS_ACTION_STOP;
}

GpsAction gps_policy_step(GpsPolicy *policy, uint64_t now)
{
    bool allowed = direct_allowed(policy);
    bool recovery_scan = allowed && policy->needs_discovery;
    if (!recovery_scan)
        policy->recovery_scan_used = false;
    if (policy->link == GPS_LINK_STOPPING || policy->link == GPS_LINK_UNCERTAIN)
        return GPS_ACTION_NONE;
    if (policy->release_requested)
        return request_stop(policy);
    if (policy->link == GPS_LINK_DISCOVERING) {
        if (!allowed || now >= policy->setup_deadline_ns)
            return request_stop(policy);
        if (!policy->discovery_complete)
            return GPS_ACTION_NONE;
        policy->link = GPS_LINK_OPENING;
        policy->needs_discovery = false;
        return GPS_ACTION_CONFIRM;
    }
    if (policy->link == GPS_LINK_OPENING)
        return !allowed || now >= policy->setup_deadline_ns ? request_stop(policy) : GPS_ACTION_NONE;
    if (policy->link == GPS_LINK_STREAMING) {
        uint64_t seen = policy->last_navigation_ns;
        bool alive = seen ? fresh(seen, now, GPS_FRAME_TIMEOUT_NS) :
                           fresh(policy->connected_ns, now, GPS_FIRST_FRAME_TIMEOUT_NS);
        return !allowed || !alive ? request_stop(policy) : GPS_ACTION_NONE;
    }
    /* A newly admitted recovery inquiry may bypass the direct-connect backoff once. */
    if (!allowed || (now < policy->retry_ns && (!recovery_scan || policy->recovery_scan_used)))
        return GPS_ACTION_NONE;
    bool discover = policy->needs_discovery || !policy->conditions.mic_installed;
    if (recovery_scan)
        policy->recovery_scan_used = true;
    policy->link = discover ? GPS_LINK_DISCOVERING : GPS_LINK_OPENING;
    policy->discovery_complete = false;
    policy->setup_deadline_ns = now + GPS_SETUP_TIMEOUT_NS;
    policy->last_navigation_ns = policy->last_fix_ns = policy->last_receipt_ns = 0;
    gps_stream_reset(&policy->stream);
    return discover ? GPS_ACTION_DISCOVER : GPS_ACTION_OPEN;
}

void gps_policy_discovered(GpsPolicy *policy)
{
    if (policy->link == GPS_LINK_DISCOVERING)
        policy->discovery_complete = true;
}

void gps_policy_connected(GpsPolicy *policy, uint64_t now)
{
    if (policy->link == GPS_LINK_OPENING) {
        policy->link = GPS_LINK_STREAMING;
        policy->connected_ns = now;
    }
}

void gps_policy_receive(GpsPolicy *policy, const uint8_t *packet, size_t size, uint64_t now)
{
    if (policy->link != GPS_LINK_STREAMING || !direct_allowed(policy) || now < policy->connected_ns ||
        now < policy->last_receipt_ns || now < policy->enabled_since_ns)
        return;
    GpsStreamResult result = gps_stream_receive(&policy->stream, &policy->cache, packet, size, now);
    if (result.gap || result.malformed)
        policy->last_navigation_ns = policy->last_fix_ns = 0;
    if (result.malformed) {
        gps_stream_reset(&policy->stream);
        return;
    }
    policy->last_receipt_ns = now;
    if (result.navigation) {
        policy->last_navigation_ns = now;
        policy->retry_seconds = 2;
    }
    if (result.fresh_fix) {
        policy->last_fix_ns = now;
        if (policy->position_source != GPS_SOURCE_BLE || !policy->trusted_from_ns)
            policy->trusted_from_ns = now;
        policy->position_source = GPS_SOURCE_BLE;
    }
}

void gps_policy_receive_hotshoe(GpsPolicy *policy, uint8_t byte, uint64_t now, bool discontinuity)
{
    if (policy->source != GPS_SOURCE_HOTSHOE || !policy->enabled ||
        !policy->conditions.awake || policy->conditions.video ||
        now < policy->connected_ns || now < policy->last_receipt_ns || now < policy->enabled_since_ns)
        return;
    if (discontinuity) {
        gps_stream_discard_partial(&policy->stream);
        return;
    }
    policy->last_receipt_ns = now;
    GpsStreamResult result = gps_stream_receive_nmea(&policy->stream, &policy->cache, &byte, 1, now);
    if (result.navigation)
        policy->last_navigation_ns = now;
    if (result.fresh_fix) {
        if (result.new_nmea_position)
            policy->last_fix_ns = now;
        if (policy->position_source != GPS_SOURCE_HOTSHOE || !policy->trusted_from_ns)
            policy->trusted_from_ns = now;
        policy->position_source = GPS_SOURCE_HOTSHOE;
    }
}

void gps_policy_stopped(GpsPolicy *policy, unsigned status, uint64_t now)
{
    bool failed_to_open = policy->link == GPS_LINK_OPENING;
    bool failed_scan = policy->link == GPS_LINK_DISCOVERING;
    policy->link = GPS_LINK_IDLE;
    policy->release_requested = false;
    policy->last_navigation_ns = policy->last_fix_ns = 0;
    policy->retry_ns = now + policy->retry_seconds * GPS_NS_PER_SECOND;
    if (status && policy->retry_seconds < 20)
        policy->retry_seconds = policy->retry_seconds >= 10 ? 20 : policy->retry_seconds * 2;
    if (failed_to_open && status == 133)
        policy->needs_discovery = true;
    if (failed_scan)
        policy->needs_discovery = false;
}

void gps_policy_bridge_lost(GpsPolicy *policy)
{
    policy->conditions.bridge_available = false;
    if (policy->link != GPS_LINK_IDLE)
        policy->link = GPS_LINK_UNCERTAIN;
    if (policy->source == GPS_SOURCE_BLE) {
        policy->last_navigation_ns = policy->last_fix_ns = 0;
        gps_stream_reset(&policy->stream);
    }
}

const char *gps_policy_live(const GpsPolicy *policy, uint64_t now)
{
    if (policy->source == GPS_SOURCE_HOTSHOE) {
        if (!policy->enabled || !policy->conditions.awake || policy->conditions.video ||
            !fresh(policy->last_receipt_ns, now, GPS_FRAME_TIMEOUT_NS) ||
            !fresh(policy->last_navigation_ns, now, GPS_FRAME_TIMEOUT_NS))
            return "hidden";
        return fresh(policy->last_fix_ns, now, GPS_FRAME_TIMEOUT_NS) ? "steady" : "blink";
    }
    if (policy->link != GPS_LINK_STREAMING || !direct_allowed(policy) ||
        !fresh(policy->last_navigation_ns, now, GPS_FRAME_TIMEOUT_NS))
        return "hidden";
    return fresh(policy->last_fix_ns, now, GPS_FRAME_TIMEOUT_NS) ? "steady" : "blink";
}

const char *gps_policy_pause_reason(const GpsPolicy *policy)
{
    const GpsConditions *c = &policy->conditions;
    if (!policy->enabled)
        return "";
    if (!c->awake)
        return "standby";
    if (c->video)
        return "video";
    if (policy->source != GPS_SOURCE_BLE)
        return "";
    if (policy->link == GPS_LINK_UNCERTAIN || !c->bridge_available)
        return "radio";
    if (!c->mic_known || !c->mic_direct)
        return "mic";
    return "";
}

const char *gps_policy_phase(const GpsPolicy *policy, uint64_t now)
{
    if (!policy->enabled)
        return "off";
    if (policy->source != GPS_SOURCE_BLE) {
        if (!policy->conditions.awake || policy->conditions.video)
            return "paused";
        if (policy->source == GPS_SOURCE_NONE)
            return "searching";
        return fresh(policy->last_fix_ns, now, GPS_FRAME_TIMEOUT_NS) ? "ready" : "acquiring";
    }
    if (!direct_allowed(policy) || policy->link == GPS_LINK_UNCERTAIN)
        return "paused";
    switch (policy->link) {
    case GPS_LINK_OPENING:
        return "connecting";
    case GPS_LINK_STREAMING:
        return fresh(policy->last_fix_ns, now, GPS_FRAME_TIMEOUT_NS) ? "ready" : "acquiring";
    case GPS_LINK_STOPPING:
        return "backoff";
    default:
        return now < policy->retry_ns ? "backoff" : "searching";
    }
}

bool gps_policy_photo_allowed(const GpsPolicy *policy, uint64_t now)
{
    const GpsConditions *c = &policy->conditions;
    if (!policy->enabled || !c->awake || c->video || !policy->trusted_from_ns ||
        policy->source != policy->position_source ||
        !gps_fix_cache_at(&policy->cache, now) ||
        !fresh(policy->last_receipt_ns, now, GPS_FRAME_TIMEOUT_NS) ||
        !fresh(policy->last_navigation_ns, now, GPS_FRAME_TIMEOUT_NS))
        return false;
    /* Accepted history remains displayable offline; photo use requires a present module. */
    if (policy->source == GPS_SOURCE_HOTSHOE)
        return true;
    return policy->source == GPS_SOURCE_BLE && policy->link == GPS_LINK_STREAMING &&
           !policy->release_requested && direct_allowed(policy);
}
