#!/bin/bash
#
# 交叉编译并部署 hisi_push 到海思设备
#
# 用法:
#   ./deploy_hisi.sh [device_ip] [user] [remote_dir]
#
# 示例:
#   ./deploy_hisi.sh 192.168.100.188 root /tmp/hisi_push
#   DEVICE_PASS=123456 ./deploy_hisi.sh 192.168.100.188 root /tmp/hisi_push
#   RUN=1 ./deploy_hisi.sh 192.168.100.188 root /tmp/hisi_push
#
# 环境变量:
#   DEVICE_IP    设备 IP, 默认 192.168.100.188
#   DEVICE_USER  登录用户, 默认 root
#   DEVICE_PASS  密码(需本机安装 sshpass), 不填则用 ssh 密钥
#   DEVICE_PORT  ssh 端口, 默认 22
#   REMOTE_DIR   设备上的目录, 默认 /tmp/hisi_push
#   RUN          1=部署后直接运行
#
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_DIR"

DEVICE_IP="${1:-${DEVICE_IP:-192.168.100.188}}"
DEVICE_USER="${2:-${DEVICE_USER:-root}}"
REMOTE_DIR="${3:-${REMOTE_DIR:-/tmp/hisi_push}}"
DEVICE_PORT="${DEVICE_PORT:-22}"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -p ${DEVICE_PORT}"
SCP_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -P ${DEVICE_PORT}"

run_ssh() {
	if [ -n "${DEVICE_PASS}" ]; then
		command -v sshpass > /dev/null || { echo "DEVICE_PASS 需要 sshpass, 请先安装"; exit 1; }
		sshpass -p "${DEVICE_PASS}" ssh ${SSH_OPTS} "${DEVICE_USER}@${DEVICE_IP}" "$@"
	else
		ssh ${SSH_OPTS} "${DEVICE_USER}@${DEVICE_IP}" "$@"
	fi
}

run_scp() {	# src... dest
	if [ -n "${DEVICE_PASS}" ]; then
		command -v sshpass > /dev/null || { echo "DEVICE_PASS 需要 sshpass, 请先安装"; exit 1; }
		sshpass -p "${DEVICE_PASS}" scp ${SCP_OPTS} "$@"
	else
		scp ${SCP_OPTS} "$@"
	fi
}

echo "==> 1/3 第三方库 (openssl/libsrtp2/usrsctp)"
if [ ! -f thirdparty/lib/hisi/lib/libsrtp2.a ] || [ ! -f thirdparty/lib/hisi/lib/libssl.a ] \
	|| [ ! -f thirdparty/lib/hisi/lib/libusrsctp.a ]; then
	./thirdparty/build_hisi_thirdparty.sh
else
	echo "    skip, thirdparty/lib/hisi 已存在"
fi

echo "==> 2/3 metaRTC 静态库"
if [ ! -f bin/lib_debug/libmetartc8.a ]; then
	./cmake_lib_hisi.sh
else
	echo "    skip, bin/lib_debug 已存在(重新编译请先删除该目录)"
fi

echo "==> 3/3 hisi_push"
./demo/hisi_push/build.sh

BIN=demo/hisi_push/build/hisi_push
INI=demo/hisi_push/hisi_push.ini
START_SH=demo/hisi_push/start.sh

echo "==> deploy to ${DEVICE_USER}@${DEVICE_IP}:${REMOTE_DIR}"

# 先确认设备可登录, 避免后面报错难定位
if ! run_ssh "true" > /dev/null 2>&1; then
	echo "无法 ssh 登录 ${DEVICE_USER}@${DEVICE_IP}:${DEVICE_PORT}"
	echo "  - 确认设备 IP/端口可访问"
	echo "  - 无密钥登录时请使用 DEVICE_PASS=xxx (需安装 sshpass)"
	exit 1
fi

run_ssh "mkdir -p '${REMOTE_DIR}'"
run_scp "${BIN}" "${INI}" "${START_SH}" "${DEVICE_USER}@${DEVICE_IP}:${REMOTE_DIR}/"
run_ssh "chmod +x '${REMOTE_DIR}/hisi_push' '${REMOTE_DIR}/start.sh'"

# 配置里引用的 h264 文件若在本地存在, 一并部署
H264=$(sed -n 's/^[[:space:]]*file[[:space:]]*=[[:space:]]*//p' "${INI}" | tail -1 | tr -d '\r')
H264=$(echo "${H264}" | sed 's/^["'\'']//; s/["'\'']$//')
if [ -n "${H264}" ]; then
	for cand in "${H264}" "demo/hisi_push/${H264}"; do
		if [ -f "${cand}" ]; then
			echo "    upload ${cand}"
			run_scp "${cand}" "${DEVICE_USER}@${DEVICE_IP}:${REMOTE_DIR}/"
			break
		fi
	done
fi

echo "==> 部署完成: ${REMOTE_DIR}"

if [ "${RUN}" = "1" ]; then
	echo "==> 前台运行(ctrl+c 退出)"
	run_ssh "cd '${REMOTE_DIR}' && ./start.sh run"
else
	echo "板端启动:"
	echo "  ssh -p ${DEVICE_PORT} ${DEVICE_USER}@${DEVICE_IP}"
	echo "  cd ${REMOTE_DIR} && ./start.sh          # 后台运行(自动重启)"
	echo "  cd ${REMOTE_DIR} && ./start.sh status"
	echo "  cd ${REMOTE_DIR} && ./start.sh stop"
fi
