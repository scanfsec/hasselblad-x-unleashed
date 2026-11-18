#!/system/bin/sh
# s_hgpsd_boot.sh - gpsd boot wrapper(zz_boot 配方; hbl_camera_service 域 via zzsh)
# 部署: /system/bin/s_hgpsd_boot.sh(755, system_file 标签)——随 service.sh install 落位
# 与 s_x2dprint 差异: 多等一处 ssd 挂载(照片在 /mnt/media_rw/ssd), 其余同配方
# (persist 门控 → pidof 幂等 → 等 dbus → nohup 拉起)。
LOG=/data/hasselblad_gps/log/boot.log
BIN=/data/hasselblad_gps/bin
ts() { date +%H:%M:%S; }
mkdir -p /data/hasselblad_gps/log /data/hasselblad_gps/run

if [ "$(getprop persist.zz.hgpsd)" != "1" ]; then
    echo "$(ts) s_hgpsd: disabled (persist.zz.hgpsd!=1), exit" >>$LOG
    exit 0
fi
if pidof gpsd >/dev/null 2>&1; then
    echo "$(ts) s_hgpsd: already running pid=$(pidof gpsd), exit" >>$LOG
    exit 0
fi
echo "$(ts) s_hgpsd: boot domain=$(id -Z 2>/dev/null || echo '?')" >>$LOG

# 等 dbus-daemon 的 socket(同为 class core, 一般 0-2s 即就绪; 上限 120s 兜底)
m=0
while [ ! -e /dev/socket/dbus ] && [ $m -lt 120 ]; do
    sleep 1; m=$((m+1))
done
echo "$(ts) s_hgpsd: dbus_wait=${m}s" >>$LOG

# 等照片盘挂载(gpsd 双轨扫描对晚挂载容错, 但 seed 最好在挂载后做; 上限 60s)
n=0
while ! grep -q ' /mnt/media_rw/ssd ' /proc/mounts 2>/dev/null && [ $n -lt 60 ]; do
    sleep 1; n=$((n+1))
done
echo "$(ts) s_hgpsd: ssd_wait=${n}s" >>$LOG

# 拉起 gpsd —— DBUS_SYSTEM_BUS_ADDRESS 必须显式注入(init 环境无该变量,
# 缺省会走 libdbus 编译期默认地址而连不上 /dev/socket/dbus, s_x2dprint P1③);
# nohup 后台化, wrapper 退出, oneshot 语义下 gpsd 常驻
DBUS_SYSTEM_BUS_ADDRESS=unix:path=/dev/socket/dbus \
  nohup $BIN/gpsd --config /data/hasselblad_gps/etc/gpsd.conf \
  >>/data/hasselblad_gps/log/stdout.log 2>&1 &
GP=$!
sleep 2
if kill -0 $GP 2>/dev/null; then
    echo "$(ts) s_hgpsd: gpsd pid=$GP, READY" >>$LOG
else
    echo "$(ts) s_hgpsd: FAILED to start, stdout.log 尾部:" >>$LOG
    tail -n 8 /data/hasselblad_gps/log/stdout.log >>$LOG 2>/dev/null
fi
