#ifndef SPLUS_GPS_SELECTION_H
#define SPLUS_GPS_SELECTION_H

#include "gps_policy.h"

#define GPS_SELECTION_BLE_ATTEMPT_NS (UINT64_C(30) * GPS_NS_PER_SECOND)
/* Two paired Mic slots can each spend 30 seconds in a startup dial. */
#define GPS_SELECTION_MIC_WAIT_NS (UINT64_C(65) * GPS_NS_PER_SECOND)
#define GPS_SELECTION_HOTSHOE_PROBE_NS GPS_FIRST_FRAME_TIMEOUT_NS

typedef enum {
    GPS_SELECTION_OFF,
    GPS_SELECTION_TRY_BLE,
    GPS_SELECTION_BLE_ACTIVE,
    GPS_SELECTION_WAIT_BLE_STOP,
    GPS_SELECTION_HOTSHOE_PROBE,
    GPS_SELECTION_HOTSHOE_ACTIVE,
    GPS_SELECTION_BACKOFF,
    GPS_SELECTION_WAIT_BLE_ADMISSION
} GpsSelectionPhase;

typedef enum {
    GPS_SELECT_NONE,
    GPS_SELECT_START_BLE,
    GPS_SELECT_STOP_BLE,
    GPS_SELECT_OPEN_HOTSHOE,
    GPS_SELECT_CLOSE_HOTSHOE
} GpsSelectionAction;

typedef struct {
    bool enabled, awake, video;
    bool ble_allowed, ble_failed, ble_mic_busy;
    GpsLinkState ble_link;
    bool hotshoe_open, hotshoe_navigation, hotshoe_io_failed;
    uint64_t hotshoe_last_byte_ns;
} GpsSelectionInput;

typedef struct {
    GpsSource source, after_ble_stop;
    GpsSelectionPhase phase;
    uint64_t started_ns, deadline_ns;
    unsigned retry_seconds;
    bool sources_absent;
    uint64_t hotshoe_last_confirmed_ns;
} GpsSelection;

/* The caller executes one returned action, then supplies the resulting resource
 * state on the next step. STOPPING/UNCERTAIN never admit the hot-shoe receiver.
 * ble_failed reports a completed failed BLE attempt or stream loss. Presence
 * after hot-shoe admission is byte activity, independent of a valid position.
 * Missing/future timestamp samples retain the current receiver's last confirmed
 * byte only until its normal 2.5-second timeout. Position/parser state belongs
 * to the caller. */
void gps_selection_init(GpsSelection *selection);
GpsSelectionAction gps_selection_step(GpsSelection *selection,
                                      const GpsSelectionInput *input, uint64_t now);
/* A confirmed offline interval expires using the newer fix/enable epoch. */
bool gps_selection_cache_expired(const GpsSelection *selection, uint64_t now,
                                  uint64_t last_valid_fix_ns, uint64_t enabled_since_ns);

#endif
