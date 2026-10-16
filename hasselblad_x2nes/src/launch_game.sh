#!/system/bin/sh
# x2nes game session on the X2D II:
#   clean weston restart -> x2nes fullscreen ROM menu (cjmlao first)
#   -> tap a ROM to play (wayland wl_touch virtual gamepad)
#   -> pad EXIT button returns to menu, menu EXIT quits
#   -> this script's watchdog then restores the stock camera GUI.
killall x2nes 2>/dev/null
killall camera-gui 2>/dev/null
setprop ctl.stop camera-gui
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
( XDG_RUNTIME_DIR=/tmp /tmp/x2nes >/tmp/x2nes.log 2>&1 & )
XP=$(pgrep x2nes | head -1)
while kill -0 $XP 2>/dev/null; do sleep 2; done
killall camera-gui 2>/dev/null
setprop ctl.stop weston
sleep 3
setprop ctl.start weston
sleep 15
setprop ctl.start camera-gui
echo STOCK_RESTORED
