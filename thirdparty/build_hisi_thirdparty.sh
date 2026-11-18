#!/bin/bash
#
# 交叉编译 metaRTC 推流所需第三方库 (arm-linux-musleabi)
#   openssl / libsrtp2 / usrsctp  ->  thirdparty/lib/hisi
#
# 用法:
#   ./thirdparty/build_hisi_thirdparty.sh
#   PREFIX=/opt/hisi-third JOBS=4 ./thirdparty/build_hisi_thirdparty.sh
#
# 可用环境变量:
#   TOOLCHAIN_BIN  工具链 bin 目录
#   CROSS          工具链前缀, 默认 arm-linux-musleabi-
#   PREFIX         安装目录, 默认 thirdparty/lib/hisi
#   SRC_DIR        源码/缓存目录, 默认 ~/.cache/metaRTC_hisi_thirdparty
#   JOBS           并行数
#   OPENSSL_SRC    本地 openssl 源码目录(可选, 有则不再下载)
#   SRTP_SRC       本地 libsrtp 源码目录(可选, 需含 configure)
#   USRSCTP_SRC    本地 usrsctp 源码目录(可选, 需含 CMakeLists.txt)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-/home/dkjar/open/arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin}"
CROSS="${CROSS:-arm-linux-musleabi-}"
PREFIX="${PREFIX:-$SCRIPT_DIR/lib/hisi}"
SRC_DIR="${SRC_DIR:-$HOME/.cache/metaRTC_hisi_thirdparty}"
JOBS="${JOBS:-$(nproc)}"

OPENSSL_VER="${OPENSSL_VER:-1.1.1w}"
SRTP_VER="${SRTP_VER:-2.5.0}"
USRSCTP_VER="${USRSCTP_VER:-0.9.5.0}"

export PATH="$TOOLCHAIN_BIN:$PATH"

if [ ! -x "${TOOLCHAIN_BIN}/${CROSS}gcc" ]; then
	echo "toolchain not found: ${TOOLCHAIN_BIN}/${CROSS}gcc"
	exit 1
fi

mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

# 第三方库不需要限制 find_root_path, 否则找不到刚装到 PREFIX 的 openssl
TC_FILE="$SRC_DIR/hisi_thirdparty_toolchain.cmake"
cat > "$TC_FILE" <<EOF
SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR arm)
SET(CMAKE_C_COMPILER ${TOOLCHAIN_BIN}/${CROSS}gcc)
SET(CMAKE_CXX_COMPILER ${TOOLCHAIN_BIN}/${CROSS}g++)
EOF

fetch() {	# url file
	local url=$1
	local out=$2

	if [ -f "$out" ] && tar -tzf "$out" > /dev/null 2>&1; then
		return 0
	fi

	rm -f "$out"
	echo "    download $url"
	curl -fL --retry 5 --retry-delay 3 --connect-timeout 15 -C - -o "$out" "$url" || true
	if ! tar -tzf "$out" > /dev/null 2>&1; then
		echo "    download failed or corrupted: $out"
		rm -f "$out"
		return 1
	fi
}

unpack() {	# tgz -> echo source dir
	local dir
	tar -tzf "$1" > /dev/null 2>&1 || { echo "bad archive $1" >&2; return 1; }
	dir=$(tar -tzf "$1" | head -1 | cut -d/ -f1)
	if [ ! -d "$SRC_DIR/$dir" ]; then
		tar -xzf "$1" -C "$SRC_DIR"
	fi
	echo "$SRC_DIR/$dir"
}

# 本地源码(不再下载), 拷贝到缓存目录后编译, 不污染原目录
local_src() {	# name candidate_dir -> echo copied dir, 失败返回1
	local name=$1
	local src=$2
	local dst="$SRC_DIR/src-$name"

	if [ -z "$src" ] || [ ! -d "$src" ]; then
		return 1
	fi
	if [ ! -d "$dst" ]; then
		echo "    use local source: $src" >&2
		cp -a "$src" "$dst"
	fi
	echo "$dst"
}

openssl_src_auto() {
	if [ -n "$OPENSSL_SRC" ]; then
		echo "$OPENSSL_SRC"
	elif [ -d "$REPO_DIR/../srs/trunk/3rdparty/openssl-1.1-fit" ]; then
		echo "$REPO_DIR/../srs/trunk/3rdparty/openssl-1.1-fit"
	elif [ -d "$HOME/open/srs/trunk/3rdparty/openssl-1.1-fit" ]; then
		echo "$HOME/open/srs/trunk/3rdparty/openssl-1.1-fit"
	fi
}

srtp_src_auto() {
	if [ -n "$SRTP_SRC" ]; then
		echo "$SRTP_SRC"
	elif [ -d "$REPO_DIR/../srs/trunk/3rdparty/libsrtp-2-fit" ]; then
		echo "$REPO_DIR/../srs/trunk/3rdparty/libsrtp-2-fit"
	elif [ -d "$HOME/open/srs/trunk/3rdparty/libsrtp-2-fit" ]; then
		echo "$HOME/open/srs/trunk/3rdparty/libsrtp-2-fit"
	fi
}

echo "==> openssl ${OPENSSL_VER}"
if [ -f "$PREFIX/lib/libssl.a" ] && [ -f "$PREFIX/lib/libcrypto.a" ]; then
	echo "    skip, already installed"
elif OSSL_DIR=$(local_src openssl "$(openssl_src_auto)") && [ -f "$OSSL_DIR/Configure" ]; then
	cd "$OSSL_DIR"
	make clean > /dev/null 2>&1 || true
else
	fetch "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/OpenSSL_${OPENSSL_VER//./_}" \
		"openssl-$OPENSSL_VER.tar.gz"
	OSSL_DIR=$(unpack "openssl-$OPENSSL_VER.tar.gz")
	cd "$OSSL_DIR"
fi
if [ ! -f "$PREFIX/lib/libssl.a" ]; then
	./Configure linux-armv4 no-asm no-shared no-tests no-async \
		--prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
		--cross-compile-prefix="$CROSS" > /dev/null
	make -j"$JOBS" > /dev/null
	make install_sw > /dev/null
fi
cd "$SRC_DIR"

echo "==> libsrtp"
if [ -f "$PREFIX/lib/libsrtp2.a" ]; then
	echo "    skip, already installed"
elif SRTP_DIR=$(local_src srtp "$(srtp_src_auto)") && [ -f "$SRTP_DIR/configure" ]; then
	cd "$SRTP_DIR"
	make distclean > /dev/null 2>&1 || true
	CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
	CFLAGS="-O2 -fPIC -fcommon" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
	./configure --host=arm-linux-musleabi --prefix="$PREFIX" \
		--enable-openssl > /dev/null
	make -j"$JOBS" > /dev/null
	make install > /dev/null
	cd "$SRC_DIR"
else
	fetch "https://codeload.github.com/cisco/libsrtp/tar.gz/refs/tags/v${SRTP_VER}" \
		"libsrtp-$SRTP_VER.tar.gz"
	SRTP_DIR=$(unpack "libsrtp-$SRTP_VER.tar.gz")
	cd "$SRTP_DIR"
	rm -rf build
	cmake -S . -B build \
		-DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DENABLE_OPENSSL=ON \
		-DENABLE_MBEDTLS=OFF \
		-DOPENSSL_ROOT_DIR="$PREFIX" \
		-DOPENSSL_USE_STATIC_LIBS=ON > /dev/null
	cmake --build build -j"$JOBS" > /dev/null
	cmake --install build > /dev/null
	cd "$SRC_DIR"
fi

echo "==> usrsctp ${USRSCTP_VER}"
if [ -f "$PREFIX/lib/libusrsctp.a" ]; then
	echo "    skip, already installed"
elif SCTP_DIR=$(local_src usrsctp "$USRSCTP_SRC") && [ -f "$SCTP_DIR/CMakeLists.txt" ]; then
	cd "$SCTP_DIR"
else
	fetch "https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/${USRSCTP_VER}" \
		"usrsctp-$USRSCTP_VER.tar.gz"
	SCTP_DIR=$(unpack "usrsctp-$USRSCTP_VER.tar.gz")
	cd "$SCTP_DIR"
fi
if [ ! -f "$PREFIX/lib/libusrsctp.a" ]; then
	rm -rf build
	cmake -S . -B build \
		-DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_BUILD_TYPE=Release \
		-Dsctp_build_shared_lib=OFF \
		-Dsctp_build_programs=OFF \
		-Dsctp_werror=OFF > /dev/null
	cmake --build build -j"$JOBS" > /dev/null
	cmake --install build > /dev/null
	cd "$SRC_DIR"
fi

echo "==> check"
failed=0
for f in libssl.a libcrypto.a libsrtp2.a libusrsctp.a; do
	if [ -f "$PREFIX/lib/$f" ]; then
		echo "    ok  $PREFIX/lib/$f"
	else
		echo "    missing $PREFIX/lib/$f"
		failed=1
	fi
done
if [ $failed -ne 0 ]; then
	exit 1
fi

echo "done: $PREFIX"
