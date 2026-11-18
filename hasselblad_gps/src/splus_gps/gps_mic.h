#ifndef SPLUS_GPS_MIC_H
#define SPLUS_GPS_MIC_H

#include "gps_policy.h"
#include <sys/types.h>

typedef enum {
    GPS_MIC_UNAVAILABLE, GPS_MIC_BLOCKED, GPS_MIC_RECORDING, GPS_MIC_DIRECT, GPS_MIC_SCAN, GPS_MIC_ABSENT
} GpsMicStatus;

GpsMicStatus gps_mic_status(pid_t bluetooth_pid);
GpsMicStatus gps_mic_parse_status(const char *line, pid_t bluetooth_pid);
void gps_mic_apply(GpsPolicy *policy, uint64_t *last_admission_ns, GpsMicStatus status, uint64_t now);

#endif
