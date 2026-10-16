#!/system/bin/sh
# robust launcher-GUI startup: retry weston+GUI until no placeholder screen
i=0
while [ $i -lt 3 ]; do
  i=$((i+1))
  killall x2nes 2>/dev/null
  killall camera-gui 2>/dev/null
  setprop ctl.stop weston
  sleep 3
  logcat -c 2>/dev/null
  setprop ctl.start weston
  n=0
  while [ $n -lt 15 ]; do
    sleep 2
    if logcat -d 2>/dev/null | grep -q "Output 'Back/EVF' enabled"; then n=99; else n=$((n+1)); fi
  done
  sleep 4
  ( XDG_RUNTIME_DIR=/tmp XDG_CACHE_HOME=/data QT_QPA_FONTDIR=/system/lib64/qt/lib/fonts LD_PRELOAD=/tmp/modqml/modgame.so /system/bin/camera-gui -platform wayland-egl --fullscreen >/tmp/modgui.log 2>&1 & )
  sleep 12
  if pgrep camera-gui >/dev/null 2>&1; then
    if grep -q placeholder /tmp/modgui.log 2>/dev/null; then
      echo "ATTEMPT $i: placeholder screen, retrying"
    else
      echo "GUI_OK attempt $i"
      exit 0
    fi
  else
    echo "ATTEMPT $i: gui died, retrying"
  fi
done
echo GUI_FAILED
exit 1
