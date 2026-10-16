#!/system/bin/sh
# x2dnes launcher: starts the mirror-loaded camera-gui (GAME tile in the
# left-swipe menu). The QML prefix race is probabilistic per GUI start, so
# this retries until the mirror main.qml loads (X2DNES_MIRROR canary), then
# reports ready.
killall x2nes 2>/dev/null
killall camera-gui 2>/dev/null
rm -rf /tmp/modqml /tmp/x2nes /tmp/roms /tmp/m.tgz 2>/dev/null

TRIES=${1:-8}
i=0
while [ $i -lt $TRIES ]; do
  i=$((i+1))
  setprop ctl.stop weston
  sleep 3
  logcat -c 2>/dev/null
  setprop ctl.start weston
  n=0
  while [ $n -lt 15 ]; do
    sleep 2
    if logcat -d 2>/dev/null | grep -q "create_shell_output"; then n=99; else n=$((n+1)); fi
  done
  sleep 6
  ( XDG_RUNTIME_DIR=/tmp XDG_CACHE_HOME=/data QML_DISABLE_DISK_CACHE=1 \
    QT_QPA_FONTDIR=/system/lib64/qt/lib/fonts \
    LD_PRELOAD=/data/x2dnes/modgame.so \
    /system/bin/camera-gui -platform wayland-egl --fullscreen >/data/x2dnes/gui.log 2>&1 & )
  sleep 10
  if ! pgrep camera-gui >/dev/null 2>&1; then
    echo "attempt $i: gui died, retrying"
    continue
  fi
  if grep -q "Failed to initialize graphics" /data/x2dnes/gui.log 2>/dev/null; then
    echo "attempt $i: GPU stale, retrying"
    continue
  fi
  if logcat -d 2>/dev/null | grep -q X2DNES_MIRROR; then
    echo "X2DNES_READY attempt $i (mirror active, GAME tile live)"
    exit 0
  fi
  echo "attempt $i: race lost, retrying"
  killall camera-gui 2>/dev/null
  sleep 1
done
echo X2DNES_FAILED
exit 1
