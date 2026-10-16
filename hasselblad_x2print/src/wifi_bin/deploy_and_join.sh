#!/bin/sh
# deploy_and_join.sh — 推送 wpa_supplicant/wpa_cli 到相机并一键加入打印机热点
# (PC 侧脚本, Windows Git Bash 下运行, 通道与 app.sh 相同: 30303 rsh + busybox nc 推送)
#
# 用法(在 x2print/ 下):
#   sh src/wifi_bin/deploy_and_join.sh join <SSID> <PSK>
#       推送 build/aarch64/{wpa_supplicant,wpa_cli} + 生成的配置 → 相机
#       /data/x2print/wifi/, 起 supplicant(-B -Dnl80211 -i wlan0), 轮询
#       wpa_cli STATUS 直到 COMPLETED(45s), 再 busybox udhcpc 取址。
#   sh src/wifi_bin/deploy_and_join.sh stop     # 相机上停 supplicant
#   sh src/wifi_bin/deploy_and_join.sh status   # 查进程/wpa_state/wlan0 IP
#   sh src/wifi_bin/deploy_and_join.sh log      # tail wpa 日志
#
# 依赖: 相机 rsh 链路通(TESTDIR/.tmpwork/{rsh.py,push.py}), LHOST 为相机侧
#       看到的本机 IP; 相机 busybox 在 /system/xbin/busybox。
set -e

TESTDIR="${TESTDIR:-C:/Users/tom/Documents/test}"
LHOST="${LHOST:-192.168.42.1}"
RSH_PY="$(cygpath -m "$TESTDIR")/.tmpwork/rsh.py"      # -m: C:/... 混合路径, Windows python 直读
PUSH_PY="$(cygpath -m "$TESTDIR")/.tmpwork/push.py"
rsh() { python "$RSH_PY" "$@"; }
push_listen() { python "$PUSH_PY" "$@"; }

HERE=$(cd "$(dirname "$0")" && pwd)            # .../x2print/src/wifi_bin
PROJ=$(cd "$HERE/../.." && pwd)                # .../x2print
BIN=$PROJ/build/aarch64
ROOT=/data/x2print/wifi                         # 相机侧部署目录

# busybox nc 推一个文件并在相机侧校验 md5(x2nes 教训: nc 会"假成功")
push_file() { # local_file remote_file
    lfile="$1"; rfile="$2"; port=$((9000 + RANDOM % 500))
    push_listen "$port" "$lfile" &
    pt=$!
    sleep 0.3
    rsh "busybox nc $LHOST $port > $rfile.tmp && chmod 755 $rfile.tmp && mv $rfile.tmp $rfile && ls -l $rfile"
    kill $pt 2>/dev/null || true
    lmd5=$(md5sum "$lfile" | cut -d' ' -f1)
    rmd5=$(rsh "busybox md5sum $rfile" | cut -d' ' -f1)
    [ "$lmd5" = "$rmd5" ] || { echo "MD5 mismatch: $rfile ($lmd5 != $rmd5)"; return 1; }
    echo "pushed $rfile md5 ok"
}

# 相机侧一键 join 脚本(rsh -f 送 stdin 执行, 免转义); $1=ROOT
cam_join='
ROOT=/data/x2print/wifi
killall wpa_supplicant 2>/dev/null; sleep 0.5
rm -rf /tmp/wpas /tmp/wpa_supplicant.pid
busybox ip link set wlan0 up 2>/dev/null || true
"$ROOT"/wpa_supplicant -B -Dnl80211 -i wlan0 -c "$ROOT"/wpa_supplicant.conf \
    -P /tmp/wpa_supplicant.pid -f "$ROOT"/wpa.log || { echo "wpa_supplicant 启动失败, tail 日志:"; tail -n 20 "$ROOT"/wpa.log; exit 1; }
sleep 1
st=""; i=0
while [ "$i" -lt 45 ]; do
    st=$("$ROOT"/wpa_cli -p /tmp/wpas status 2>/dev/null | grep "^wpa_state=" | cut -d= -f2)
    echo "[poll $i] wpa_state=${st:-<supplicant 未就绪>}"
    [ "$st" = "COMPLETED" ] && break
    i=$((i + 1)); sleep 1
done
if [ "$st" != "COMPLETED" ]; then
    echo "JOIN FAILED(最后状态: ${st:-none}), tail wpa.log:"
    tail -n 25 "$ROOT"/wpa.log
    exit 1
fi
echo "== 4 次握手完成, udhcpc 取址 =="
# udhcpc 事件脚本缺失时现场生成最小版(deconfig/bound/renew)
if [ ! -f /tmp/udhcpc.sh ]; then
    printf "#!/system/bin/sh\ncase $1 in deconfig) busybox ifconfig $interface 0.0.0.0;; bound|renew) busybox ifconfig $interface $ip netmask $subnet; busybox route add default gw $router 2>/dev/null;; esac\n" > /tmp/udhcpc.sh
    chmod 755 /tmp/udhcpc.sh
    echo "(提示: 相机无 /tmp/udhcpc.sh, 已生成最小事件脚本)"
fi
busybox udhcpc -i wlan0 -s /tmp/udhcpc.sh -n -q -t 6 -T 3 || echo "警告: udhcpc 未取到租约"
echo "== 结果 =="
"$ROOT"/wpa_cli -p /tmp/wpas status | grep -E "^(wpa_state|ssid|bssid|key_mgmt)=" || true
busybox ifconfig wlan0 2>/dev/null | grep -E "inet |HWaddr" || echo "(wlan0 无 IP)"
'

case "${1:-}" in
""|-h|--help|help)
    sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
    ;;
join)
    SSID="${2:-}"; PSK="${3:-}"
    [ -n "$SSID" ] && [ -n "$PSK" ] || { echo "用法: $0 join <SSID> <PSK>"; exit 2; }
    # 注意用 -f 不用 -x: WSL 产出文件在 Git Bash 下不带 x 位(推送后相机侧 chmod 755)
    [ -f "$BIN/wpa_supplicant" ] && [ -f "$BIN/wpa_cli" ] || {
        echo "缺 $BIN/{wpa_supplicant,wpa_cli}: 先跑 third_party/build_wpa.sh"; exit 1; }
    rsh "mkdir -p $ROOT"
    push_file "$BIN/wpa_supplicant" "$ROOT/wpa_supplicant"
    push_file "$BIN/wpa_cli"      "$ROOT/wpa_cli"
    # 模板替换 {{SSID}}/{{PSK}}(python 做纯文本替换, 免 sed 元字符坑)
    # 临时文件放脚本目录内: Windows python 打不开 MSYS /tmp 路径
    TMP="$HERE/.wpa_conf.tmp"
    SSID="$SSID" PSK="$PSK" python - "$HERE/wpa_supplicant.conf" > "$TMP" <<'PYEOF'
import os, sys
tpl = open(sys.argv[1], encoding="utf-8").read()
sys.stdout.write(tpl.replace("{{SSID}}", os.environ["SSID"]).replace("{{PSK}}", os.environ["PSK"]))
PYEOF
    push_file "$TMP" "$ROOT/wpa_supplicant.conf"
    rm -f "$TMP"
    # 相机侧 join 脚本落盘后经 rsh -f 送入(rsh.py 不认 /dev/stdin)
    printf '%s\n' "$cam_join" > "$HERE/.cam_join.tmp"
    rsh -f "$HERE/.cam_join.tmp"
    rm -f "$HERE/.cam_join.tmp"
    ;;
stop)
    rsh "killall wpa_supplicant 2>/dev/null; rm -rf /tmp/wpas /tmp/wpa_supplicant.pid; echo stopped"
    ;;
status)
    rsh "ps -A 2>/dev/null | grep -v grep | grep wpa_; \
$ROOT/wpa_cli -p /tmp/wpas status 2>/dev/null | grep -E '^(wpa_state|ssid|bssid|key_mgmt)=' || echo '(supplicant 未运行)'; \
busybox ifconfig wlan0 2>/dev/null | grep -E 'inet |HWaddr'"
    ;;
log)
    rsh "tail -n ${2:-40} $ROOT/wpa.log 2>/dev/null || echo '(无日志)'"
    ;;
*)
    echo "未知子命令: $1(可用: join/stop/status/log)" >&2; exit 2
    ;;
esac
