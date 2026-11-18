#!/bin/bash
#
# 交叉编译 metaRTC 核心静态库 (Android)
#   yangutil8 / metartccore8 / yangwhip8 / metartc8
#   产物输出到 bin/lib_android/<abi>
#
# 前置:
#   ./thirdparty/build_android_thirdparty.sh    # openssl / libsrtp2 / usrsctp
#
# 用法:
#   ./cmake_lib_android.sh
#   ABI=arm64-v8a ANDROID_API=24 ./cmake_lib_android.sh
#
# 可用环境变量:
#   ANDROID_NDK   NDK 根目录, 默认自动探测
#   ABI           默认 arm64-v8a
#   ANDROID_API   默认 29
#   JOBS          并行数
#
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_DIR"

detect_ndk() {
	if [ -n "$ANDROID_NDK" ]; then
		echo "$ANDROID_NDK"
		return
	fi
	local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
	local latest
	latest=$(ls -1d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)
	if [ -n "$latest" ]; then
		echo "$latest"
		return
	fi
	echo "$sdk/ndk/25.1.8937393"
}

NDK="$(detect_ndk)"
ABI="${ABI:-arm64-v8a}"
ANDROID_API="${ANDROID_API:-29}"
JOBS="${JOBS:-$(nproc)}"

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
	echo "NDK toolchain not found: $TOOLCHAIN"
	echo "请设置 ANDROID_NDK 指向 NDK 根目录"
	exit 1
fi

OUT_DIR="$REPO_DIR/bin/lib_android/$ABI"
BUILD_ROOT="$REPO_DIR/build_android/$ABI"
mkdir -p "$OUT_DIR" "$BUILD_ROOT"

echo "==> NDK : $NDK"
echo "==> ABI : $ABI (api $ANDROID_API)"
echo "==> OUT : $OUT_DIR"

build_lib() {	# dir name
	local dir=$1
	local name=$2
	local bdir="$BUILD_ROOT/$name"

	echo "==> $name"
	cmake -S "$REPO_DIR/$dir" -B "$bdir" \
		-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DANDROID_NDK="$NDK" \
		-DANDROID_ABI="$ABI" \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DANDROID_STL=c++_static \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_STANDARD=11 \
		-DCMAKE_CXX_STANDARD=11 \
		-DNoPlayer=ON > /dev/null

	cmake --build "$bdir" -j"$JOBS" > /dev/null

	local lib
	lib=$(find "$bdir" -maxdepth 2 -name "lib${name}.a" | head -1)
	if [ -z "$lib" ]; then
		echo "    build failed: lib${name}.a not found"
		exit 1
	fi
	cp "$lib" "$OUT_DIR/"
	echo "    $(basename "$lib")"
}

build_lib libyangutil8   yangutil8
build_lib libmetartccore8 metartccore8
build_lib libyangwhip8   yangwhip8
build_lib libmetartc8    metartc8

echo ""
echo "==> done: $OUT_DIR"
ls -lh "$OUT_DIR" | awk 'NR>1{print "    "$9" "$5}'
