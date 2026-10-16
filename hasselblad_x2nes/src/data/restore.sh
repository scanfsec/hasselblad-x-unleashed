#!/system/bin/sh
# back to stock camera (remove LD_PRELOAD gui, restart services)
killall x2nes 2>/dev/null
killall camera-gui 2>/dev/null
rm -rf /tmp/modqml /tmp/x2nes /tmp/roms 2>/dev/null
setprop ctl.stop weston; sleep 3; setprop ctl.start weston; sleep 15
setprop ctl.start camera-gui
echo STOCK_RESTORED
