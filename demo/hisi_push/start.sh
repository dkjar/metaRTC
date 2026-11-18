#!/bin/sh
#
# hisi_push 板端启动脚本 (busybox /bin/sh 兼容)
#
# 用法:
#   ./start.sh            后台启动(默认, 异常退出自动重启)
#   ./start.sh run        前台运行, 直接看输出(调试用)
#   ./start.sh stop       停止
#   ./start.sh restart    重启
#   ./start.sh status     查看状态
#
# 环境变量:
#   CONFIG     配置文件, 默认脚本目录下 hisi_push.ini
#   LOG_FILE   日志文件, 默认脚本目录下 hisi_push.log
#   RESTART    1=退出后自动重启(默认), 0=不重启
#
# 注意: 日志和 pid 文件写在脚本目录, 请放在可写分区(如 /tmp、/userdata)
#

DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$DIR/hisi_push"
CONFIG="${CONFIG:-$DIR/hisi_push.ini}"
LOG_FILE="${LOG_FILE:-$DIR/hisi_push.log}"
PID_FILE="$DIR/hisi_push.pid"
RESTART="${RESTART:-1}"
MAX_LOG_SIZE=1048576
child_pid=""

log_rotate() {
	[ -f "$LOG_FILE" ] || return 0
	size=$(wc -c < "$LOG_FILE" 2>/dev/null)
	[ -n "$size" ] && [ "$size" -gt "$MAX_LOG_SIZE" ] && mv -f "$LOG_FILE" "$LOG_FILE.1"
	return 0
}

loop() {
	while :; do
		log_rotate
		echo "[$(date '+%F %T')] start hisi_push, config=$CONFIG" >> "$LOG_FILE"
		"$BIN" "$CONFIG" >> "$LOG_FILE" 2>&1 &
		child_pid=$!
		wait "$child_pid"
		code=$?
		child_pid=""
		echo "[$(date '+%F %T')] hisi_push exit, code=$code" >> "$LOG_FILE"
		[ "$RESTART" = "1" ] || break
		sleep 3
	done
}

is_running() {
	[ -f "$PID_FILE" ] || return 1
	pid=$(cat "$PID_FILE" 2>/dev/null)
	[ -n "$pid" ] || return 1
	kill -0 "$pid" 2>/dev/null
}

case "$1" in
run)
	[ -x "$BIN" ] || { echo "cannot exec $BIN"; exit 1; }
	[ -f "$CONFIG" ] || { echo "config not found: $CONFIG"; exit 1; }
	exec "$BIN" "$CONFIG"
	;;
start|"")
	[ -x "$BIN" ] || { echo "cannot exec $BIN"; exit 1; }
	[ -f "$CONFIG" ] || { echo "config not found: $CONFIG"; exit 1; }
	if is_running; then
		echo "already running, pid=$(cat "$PID_FILE")"
		exit 0
	fi
	nohup sh "$0" __loop >> "$LOG_FILE" 2>&1 &
	echo $! > "$PID_FILE"
	sleep 1
	if is_running; then
		echo "started, pid=$(cat "$PID_FILE")"
		echo "log: $LOG_FILE"
	else
		echo "start failed, check $LOG_FILE"
		exit 1
	fi
	;;
stop)
	if is_running; then
		pid=$(cat "$PID_FILE")
		kill "$pid" 2>/dev/null
		for i in 1 2 3 4 5; do
			is_running || break
			sleep 1
		done
		if is_running; then
			kill -9 "$pid" 2>/dev/null
		fi
		echo "stopped"
	else
		echo "not running"
	fi
	rm -f "$PID_FILE"
	;;
restart)
	"$0" stop
	sleep 1
	exec "$0" start
	;;
status)
	if is_running; then
		echo "running, pid=$(cat "$PID_FILE")"
		echo "config: $CONFIG"
		echo "log: $LOG_FILE"
	else
		echo "not running"
	fi
	;;
__loop)
	trap 'kill "$child_pid" 2>/dev/null; exit 0' TERM INT
	loop
	;;
*)
	echo "usage: $0 [start|stop|restart|status|run]"
	exit 1
	;;
esac
