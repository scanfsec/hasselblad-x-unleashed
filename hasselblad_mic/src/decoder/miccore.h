/*
 * miccore.h -- DJI Mic 2 in-process engine (stage 1: frida-injected).
 *
 * Single feed-style entry point: the host (frida hook in stage 1, a read()
 * interposer in stage 2) pushes raw SPP byte chunks into miccore_feed();
 * the engine owns everything else:
 *   - Wl block stream reassembly (55 77 05 sync, checksum, cmd dispatch)
 *   - LC3plus decode (48k/mono/5ms -> 240 s16 samples per 111B frame)
 *   - PCM FIFO /data/mic/mic.pcm (non-blocking, drop on EAGAIN)
 *   - status JSON /data/mic/mic_status.json (atomic, 1Hz + events)
 *   - control socket /data/mic/mic_ctl (INTERFACE.md line protocol)
 *   - auto-connect via in-process bt_bsa_app::app_dg_open_ex2
 *   - scan/pair/unpair via the bsa_server daemon socket
 *
 * The engine never kills or restarts any system process. It is dlopen()ed
 * into bt_bsa_app by the native injector/hook package. The original Frida
 * loader is retained as a legacy tool; readhook.c is an unused prototype.
 */
#ifndef MICCORE_H
#define MICCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent start. Creates FIFO/status/control faces, spawns housekeeper
 * thread, loads decoder, reads /data/mic/mic.cfg (bd=..., auto_connect=1).
 * Returns 0 on success, negative errno-ish on failure. */
int  miccore_start(void);

/* For the Frida development loader after successful start. Returns the
 * ABI-discovered address-bearing native callback, or NULL outside bt_bsa_app.
 * No loader may substitute a hard-coded offset after discovery fails. */
void *miccore_native_play_data(void);

/* Push raw SPP stream bytes (anything that flowed through
 * bt_bsa_app::app_audio_play_data). Safe to call from any thread;
 * must be fast (runs on the UIPC read path of bt_bsa_app). */
void miccore_feed(const uint8_t *buf, int len);

/* Address-bearing feed. bd points to six bytes from the transport callback,
 * valid for this call only; the engine copies it before retaining any input.
 * NULL uses the legacy single-target entry and has unverified attribution.
 * Unknown addresses are rejected, never silently assigned to another TX. */
void miccore_feed_bd(const uint8_t bd[6], const uint8_t *buf, int len);

/* SPP connect to the only configured bd via in-process app_dg_open_ex2.
 * Returns 0 if requested, 1 if streaming/control busy, 2 if connecting,
 * -3 if explicit pairing is required, -4 if disconnect is pending,
 * -5 if two targets require an address, -6 if the bond database is temporarily
 * unavailable, other negative on failure.
 * Never rotates a bond. For two configured peers, use connect <bd> on
 * the control socket. The no-address C controls select TX2 if it is alone. */
int  miccore_connect(void);

/* SPP disconnect: wait for native slot retirement; a submitted close alone
 * is not success. BSA TmDisconnect is a fallback without an in-process table.
 * Returns -5 for two configured peers; use disconnect <bd> on the socket. */
int  miccore_disconnect(void);

/* Render legacy status (TX1, or TX2 if it is the only configured slot).
 * Returns length, or negative on error. The socket commands status <bd>
 * and status_all expose explicit targets and both slots respectively. */
int  miccore_status_json(char *out, int outsz);

/* WAV recorder control (used by the `rec`/`stop` control commands and by
 * the offline self-test). sec==0 means no auto-stop. */
int  miccore_rec_start(int sec, const char *path);
int  miccore_rec_stop(char *path_out, int path_sz, uint64_t *bytes_out);

/* Kickstart ladder for TX streaming state (ccode + sync-addr/link-up),
 * callable manually; also invoked automatically after connect(). */
int  miccore_kickstart(void);

/* Stop engine (test/teardown only; not used in production). */
int  miccore_shutdown(void);
int  miccore_in_use(void); /* leases or any active local recorder */
int  miccore_stop_if_idle(void); /* 1 = retained for an active owner */

#ifdef __cplusplus
}
#endif

#endif /* MICCORE_H */
