#!/system/bin/sh
# x2dnes game session (EGL build - stable, no weston restarts needed):
# stop stock GUI -> x2nes fullscreen (joystick/shutter/M + touch) -> restore
killall x2nes 2>/dev/null
setprop ctl.stop camera-gui
sleep 2
( XDG_RUNTIME_DIR=/tmp /data/x2dnes/x2nes >/data/x2dnes/x2nes.log 2>&1 & )
while pgrep -x x2nes >/dev/null || pgrep -x xdoom >/dev/null; do sleep 2; done
setprop ctl.start camera-gui
echo GAME_DONE_GUI_RESTORED
