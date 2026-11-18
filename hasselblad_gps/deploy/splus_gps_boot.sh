#!/system/bin/sh
set -eu

root=/data/hasselblad-gps
run=$root/run
[ -f "$root/service.enabled" ] || exit 0
umask 077
mkdir -p "$run"
[ ! -L "$root" ] && [ ! -L "$run" ]
[ "$(stat -c '%u:%a' "$root")" = 0:700 ] && [ "$(stat -c '%u:%a' "$run")" = 0:700 ]
exec 7>"$run/supervisor.lock"
flock -n 0 <&7 || exit 0

release=$(cat "$root/active-release")
[ "${#release}" = 64 ]
case "$release" in *[!0-9a-f]*) exit 1;; esac
package=$root/releases/$release
[ ! -L "$package" ] && [ "$(stat -c '%u:%a' "$package")" = 0:700 ]
digest() { sha256sum "$1" | cut -d ' ' -f 1; }
[ "$(digest "$package/SHA256SUMS")" = "$release" ]
for file in splus-gpsd libgps_bsa.so gps-bsa-load splus_gps_boot.sh splus_gps.rc; do
    [ -f "$package/$file" ] && [ ! -L "$package/$file" ]
    [ "$(digest "$package/$file")" = "$(awk -v f="$file" '$2==f {print $1}' "$package/SHA256SUMS")" ]
done
[ "$(cat /system/etc/VERSION)" = 1.3.18.1 ]
[ "$(getprop ro.build.version.incremental)" = 343 ]
[ "$(digest /system/bin/bt_bsa_app)" = d13a10643e53bcc78036c67411dd0550ab3bea858309e03288c564b6183878f0 ]
[ "$(digest /system/bin/bsa_server)" = eb11ceb1db370248907ab7740444ab4227c7dd5f02ad3ed88eb17d309009c26f ]

export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/dev/socket/dbus
while [ ! -S /dev/socket/dbus ] && [ -f "$root/service.enabled" ]; do sleep 1; done
[ -f "$root/service.enabled" ] || exit 0

daemon=''
cleanup() {
    if [ -n "$daemon" ]; then
        kill -TERM "$daemon" 2>/dev/null || true
        wait "$daemon" || true
    fi
}
trap cleanup EXIT
trap 'exit 0' INT TERM HUP
boot=$(cat /proc/sys/kernel/random/boot_id)
ensure_bridge() {
    [ -f "$root/enabled" ] || return 0
    pid=$(pidof bt_bsa_app || true)
    case "$pid" in ''|*[!0-9]*|0|1) return 0;; esac
    [ "$(readlink "/proc/$pid/exe" || true)" = /system/bin/bt_bsa_app ] || return 0
    start=$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)
    case "$start" in ''|*[!0-9]*|0) return 0;; esac
    identity="$boot $pid $start"
    previous=$(cat "$run/bridge-attempt" 2>/dev/null || true)
    [ "$identity" != "$previous" ] || return 0
    if grep -q ' /data/hasselblad-gps/releases/.*libgps_bsa.so' "/proc/$pid/maps"; then
        return 0
    fi
    if [ -S "$run/bsa.sock" ]; then
        if grep -Fq " $run/bsa.sock" /proc/net/unix; then return 0; fi
        rm "$run/bsa.sock"
    fi
    [ "$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null || true)" = "$start" ] || return 0
    printf '%s\n' "$identity" > "$run/bridge-attempt.new"
    mv "$run/bridge-attempt.new" "$run/bridge-attempt"
    # Never interrupt an in-flight native load. Its outcome is pinned to this process.
    result=0
    (exec 7>&-; "$package/gps-bsa-load" "$pid" "$package/libgps_bsa.so") || result=$?
    # A shared loader lock refusal happens before native attachment.
    if [ "$result" -eq 4 ]; then rm "$run/bridge-attempt"; fi
}

while [ -f "$root/service.enabled" ]; do
    if [ -S /dev/socket/dbus ]; then
        (exec 7>&-; exec "$package/splus-gpsd") &
        daemon=$!
        while [ -f "$root/service.enabled" ] && kill -0 "$daemon" 2>/dev/null; do
            ensure_bridge
            sleep 2
        done
        [ -f "$root/service.enabled" ] || break
        wait "$daemon" || true
        daemon=''
    fi
    sleep 5
done
