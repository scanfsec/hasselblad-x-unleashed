#include "gps_selection.h"

void gps_selection_init(GpsSelection *selection)
{
    *selection = (GpsSelection){.source = GPS_SOURCE_NONE,
                               .after_ble_stop = GPS_SOURCE_NONE,
                               .retry_seconds = 2};
}

bool gps_selection_cache_expired(const GpsSelection *selection, uint64_t now,
                                  uint64_t last_valid_fix_ns, uint64_t enabled_since_ns)
{
    uint64_t anchor = last_valid_fix_ns > enabled_since_ns ? last_valid_fix_ns : enabled_since_ns;
    return selection->sources_absent && anchor && anchor <= now && now - anchor >= GPS_FIX_TTL_NS;
}

static GpsSelectionAction start_ble(GpsSelection *selection, const GpsSelectionInput *input, uint64_t now)
{
    selection->source = GPS_SOURCE_BLE;
    selection->phase = input->ble_allowed ? GPS_SELECTION_TRY_BLE : GPS_SELECTION_WAIT_BLE_ADMISSION;
    selection->started_ns = now;
    selection->deadline_ns = now + (input->ble_mic_busy ? GPS_SELECTION_MIC_WAIT_NS : GPS_SELECTION_BLE_ATTEMPT_NS);
    return GPS_SELECT_START_BLE;
}

static GpsSelectionAction open_hotshoe(GpsSelection *selection, uint64_t now)
{
    selection->source = GPS_SOURCE_HOTSHOE;
    selection->phase = GPS_SELECTION_HOTSHOE_PROBE;
    selection->started_ns = now;
    selection->deadline_ns = now + GPS_SELECTION_HOTSHOE_PROBE_NS;
    selection->hotshoe_last_confirmed_ns = 0;
    return GPS_SELECT_OPEN_HOTSHOE;
}

static GpsSelectionAction fallback_hotshoe(GpsSelection *selection,
                                          const GpsSelectionInput *input, uint64_t now)
{
    if (input->ble_link == GPS_LINK_IDLE)
        return open_hotshoe(selection, now);
    selection->source = GPS_SOURCE_NONE;
    selection->phase = GPS_SELECTION_WAIT_BLE_STOP;
    selection->after_ble_stop = GPS_SOURCE_HOTSHOE;
    selection->deadline_ns = 0;
    return input->ble_link == GPS_LINK_STOPPING || input->ble_link == GPS_LINK_UNCERTAIN ?
           GPS_SELECT_NONE : GPS_SELECT_STOP_BLE;
}

static GpsSelectionAction backoff(GpsSelection *selection,
                                  const GpsSelectionInput *input, uint64_t now)
{
    selection->source = GPS_SOURCE_NONE;
    selection->phase = GPS_SELECTION_BACKOFF;
    selection->deadline_ns = now + selection->retry_seconds * GPS_NS_PER_SECOND;
    selection->retry_seconds = selection->retry_seconds >= 10 ? 20 : selection->retry_seconds * 2;
    selection->hotshoe_last_confirmed_ns = 0;
    return input->hotshoe_open ? GPS_SELECT_CLOSE_HOTSHOE : GPS_SELECT_NONE;
}

static bool hotshoe_alive(GpsSelection *selection,
                          const GpsSelectionInput *input, uint64_t now)
{
    uint64_t stamp = input->hotshoe_last_byte_ns;
    if (stamp >= selection->started_ns && stamp <= now && stamp > selection->hotshoe_last_confirmed_ns)
        selection->hotshoe_last_confirmed_ns = stamp;
    stamp = selection->hotshoe_last_confirmed_ns;
    return stamp && stamp <= now && now - stamp < GPS_FRAME_TIMEOUT_NS;
}

GpsSelectionAction gps_selection_step(GpsSelection *selection,
                                      const GpsSelectionInput *input, uint64_t now)
{
    if (!input->enabled || !input->awake || input->video) {
        selection->source = GPS_SOURCE_NONE;
        selection->phase = GPS_SELECTION_OFF;
        selection->after_ble_stop = GPS_SOURCE_NONE;
        selection->started_ns = selection->deadline_ns = 0;
        selection->retry_seconds = 2;
        selection->sources_absent = false;
        selection->hotshoe_last_confirmed_ns = 0;
        if (input->hotshoe_open)
            return GPS_SELECT_CLOSE_HOTSHOE;
        return input->ble_link != GPS_LINK_IDLE && input->ble_link != GPS_LINK_STOPPING &&
               input->ble_link != GPS_LINK_UNCERTAIN ? GPS_SELECT_STOP_BLE : GPS_SELECT_NONE;
    }
    if (input->hotshoe_open && selection->source != GPS_SOURCE_HOTSHOE)
        return GPS_SELECT_CLOSE_HOTSHOE;

    switch (selection->phase) {
    case GPS_SELECTION_OFF:
        if (input->ble_link == GPS_LINK_IDLE)
            return start_ble(selection, input, now);
        selection->source = GPS_SOURCE_NONE;
        selection->phase = GPS_SELECTION_WAIT_BLE_STOP;
        selection->after_ble_stop = GPS_SOURCE_BLE;
        return input->ble_link == GPS_LINK_STOPPING || input->ble_link == GPS_LINK_UNCERTAIN ?
               GPS_SELECT_NONE : GPS_SELECT_STOP_BLE;

    case GPS_SELECTION_WAIT_BLE_ADMISSION:
        if (input->ble_link != GPS_LINK_IDLE || input->ble_failed || now >= selection->deadline_ns)
            return fallback_hotshoe(selection, input, now);
        if (input->ble_allowed) {
            selection->phase = GPS_SELECTION_TRY_BLE;
            selection->deadline_ns = now + GPS_SELECTION_BLE_ATTEMPT_NS;
        } else if (input->ble_mic_busy) {
            selection->deadline_ns = selection->started_ns + GPS_SELECTION_MIC_WAIT_NS;
        }
        return GPS_SELECT_NONE;

    case GPS_SELECTION_TRY_BLE:
        if (!input->ble_allowed || input->ble_failed ||
            input->ble_link == GPS_LINK_STOPPING || input->ble_link == GPS_LINK_UNCERTAIN)
            return fallback_hotshoe(selection, input, now);
        if (input->ble_link == GPS_LINK_STREAMING) {
            selection->sources_absent = false;
            selection->phase = GPS_SELECTION_BLE_ACTIVE;
            selection->deadline_ns = 0;
            selection->retry_seconds = 2;
        } else if (now >= selection->deadline_ns) {
            return fallback_hotshoe(selection, input, now);
        }
        return GPS_SELECT_NONE;

    case GPS_SELECTION_BLE_ACTIVE:
        if (!input->ble_allowed || input->ble_failed || input->ble_link != GPS_LINK_STREAMING)
            return fallback_hotshoe(selection, input, now);
        selection->sources_absent = false;
        return GPS_SELECT_NONE;

    case GPS_SELECTION_WAIT_BLE_STOP:
        if (input->ble_link != GPS_LINK_IDLE)
            return GPS_SELECT_NONE;
        return selection->after_ble_stop == GPS_SOURCE_BLE ?
               start_ble(selection, input, now) : open_hotshoe(selection, now);

    case GPS_SELECTION_HOTSHOE_PROBE:
        if (input->hotshoe_io_failed) {
            selection->sources_absent = true;
            return backoff(selection, input, now);
        }
        bool alive = hotshoe_alive(selection, input, now);
        if (input->hotshoe_open && input->hotshoe_navigation && alive) {
            selection->sources_absent = false;
            selection->phase = GPS_SELECTION_HOTSHOE_ACTIVE;
            selection->deadline_ns = 0;
            selection->retry_seconds = 2;
        } else if (now >= selection->deadline_ns) {
            selection->sources_absent = true;
            return backoff(selection, input, now);
        }
        return GPS_SELECT_NONE;

    case GPS_SELECTION_HOTSHOE_ACTIVE:
        if (input->hotshoe_io_failed || !input->hotshoe_open || !hotshoe_alive(selection, input, now))
            return backoff(selection, input, now);
        selection->sources_absent = false;
        return GPS_SELECT_NONE;

    case GPS_SELECTION_BACKOFF:
        if (input->ble_link != GPS_LINK_IDLE) {
            selection->phase = GPS_SELECTION_WAIT_BLE_STOP;
            selection->after_ble_stop = GPS_SOURCE_BLE;
            return input->ble_link == GPS_LINK_STOPPING || input->ble_link == GPS_LINK_UNCERTAIN ?
                   GPS_SELECT_NONE : GPS_SELECT_STOP_BLE;
        }
        return now >= selection->deadline_ns ? start_ble(selection, input, now) : GPS_SELECT_NONE;
    }
    return GPS_SELECT_NONE;
}
