#!/bin/bash
#
# 交叉编译 metaRTC Android 推拉流所需第三方库
#   openssl / libsrtp2 / usrsctp  ->  thirdparty/lib/android/<abi>
#
# 用法:
#   ./thirdparty/build_android_thirdparty.sh
#   ABI=arm64-v8a ANDROID_API=24 ./thirdparty/build_android_thirdparty.sh
#
# 可用环境变量:
#   ANDROID_NDK   NDK 根目录, 默认自动探测 ~/Android/Sdk/ndk/25.1.8937393
#   ABI           目标 ABI, 默认 arm64-v8a (可选 armeabi-v7a / x86_64)
#   ANDROID_API   最低 API, 默认 24
#   PREFIX        安装目录, 默认 thirdparty/lib/android/<abi>
#   SRC_DIR       源码缓存目录, 默认 ~/.cache/metaRTC_android_thirdparty
#   JOBS          并行数
#
# 说明: 复用海思脚本已下载的源码缓存(若存在), 避免重复下载
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

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
PREFIX="${PREFIX:-$SCRIPT_DIR/lib/android/$ABI}"
SRC_DIR="${SRC_DIR:-$HOME/.cache/metaRTC_android_thirdparty}"
JOBS="${JOBS:-$(nproc)}"

OPENSSL_VER="${OPENSSL_VER:-1.1.1w}"
SRTP_VER="${SRTP_VER:-2.5.0}"
USRSCTP_VER="${USRSCTP_VER:-0.9.5.0}"
OPUS_VER="${OPUS_VER:-1.3.1}"
FFMPEG_VER="${FFMPEG_VER:-5.1.4}"

HISI_CACHE="$HOME/.cache/metaRTC_hisi_thirdparty"

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
	echo "NDK toolchain not found: $TOOLCHAIN"
	echo "请设置 ANDROID_NDK 指向 NDK 根目录"
	exit 1
fi

case "$ABI" in
	arm64-v8a)   OSSL_TARGET=android-arm64;  OPUS_HOST=aarch64-linux-android;     FFMPEG_ARCH=aarch64; FFMPEG_CPU=armv8-a ;;
	armeabi-v7a) OSSL_TARGET=android-arm;    OPUS_HOST=armv7a-linux-androideabi; FFMPEG_ARCH=arm;     FFMPEG_CPU=armv7-a ;;
	x86_64)      OSSL_TARGET=android-x86_64; OPUS_HOST=x86_64-linux-android;      FFMPEG_ARCH=x86_64;  FFMPEG_CPU=x86-64  ;;
	*) echo "unsupported ABI: $ABI"; exit 1 ;;
esac

echo "==> NDK      : $NDK"
echo "==> ABI      : $ABI (api $ANDROID_API)"
echo "==> PREFIX   : $PREFIX"

mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

fetch() {	# url file
	local url=$1
	local out=$2

	if [ -f "$out" ] && tar -tf "$out" > /dev/null 2>&1; then
		return 0
	fi
	rm -f "$out"
	echo "    download $url"
	curl -fL --retry 5 --retry-delay 3 --connect-timeout 15 -C - -o "$out" "$url" || true
	if ! tar -tf "$out" > /dev/null 2>&1; then
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

# 复用已下载的海思源码缓存
reuse_cache() {	# name candidate_dir -> echo copied dir
	local name=$1
	local src=$2
	local dst="$SRC_DIR/src-$name"

	if [ -z "$src" ] || [ ! -d "$src" ]; then
		return 1
	fi
	if [ ! -d "$dst" ]; then
		echo "    reuse cached source: $src" >&2
		cp -a "$src" "$dst"
	fi
	echo "$dst"
}

# ---------------------------------------------------------------- openssl
echo "==> openssl $OPENSSL_VER"
if [ -f "$PREFIX/lib/libssl.a" ] && [ -f "$PREFIX/lib/libcrypto.a" ]; then
	echo "    skip, already installed"
else
	OSSL_DIR=""
	if OSSL_DIR=$(reuse_cache openssl "$HISI_CACHE/src-openssl") && [ -f "$OSSL_DIR/Configure" ]; then
		:
	elif OSSL_DIR=$(reuse_cache openssl "$HISI_CACHE/openssl-OpenSSL_${OPENSSL_VER//./_}" 2>/dev/null) && [ -f "$OSSL_DIR/Configure" ]; then
		:
	else
		fetch "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/OpenSSL_${OPENSSL_VER//./_}" \
			"openssl-$OPENSSL_VER.tar.gz"
		OSSL_DIR=$(unpack "openssl-$OPENSSL_VER.tar.gz")
	fi

	cd "$OSSL_DIR"
	make clean > /dev/null 2>&1 || true
	# OpenSSL 1.1.1 的 android-* 目标通过 ANDROID_NDK_HOME / ANDROID_NDK_ROOT 找 NDK 工具链
	export ANDROID_NDK_HOME="$NDK"
	export ANDROID_NDK_ROOT="$NDK"
	export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
	./Configure "$OSSL_TARGET" no-asm no-shared no-tests no-async \
		-D__ANDROID_API__="$ANDROID_API" \
		--prefix="$PREFIX" --openssldir="$PREFIX/ssl" > /dev/null
	make -j"$JOBS" > /dev/null
	make install_sw > /dev/null
	cd "$SRC_DIR"
fi

# ---------------------------------------------------------------- libsrtp2
echo "==> libsrtp2 $SRTP_VER"
if [ -f "$PREFIX/lib/libsrtp2.a" ]; then
	echo "    skip, already installed"
else
	if SRTP_DIR=$(reuse_cache srtp "$HISI_CACHE/src-srtp") && [ -f "$SRTP_DIR/CMakeLists.txt" ]; then
		:
	else
		fetch "https://codeload.github.com/cisco/libsrtp/tar.gz/refs/tags/v${SRTP_VER}" \
			"libsrtp-$SRTP_VER.tar.gz"
		SRTP_DIR=$(unpack "libsrtp-$SRTP_VER.tar.gz")
	fi

	cd "$SRTP_DIR"
	rm -rf build && mkdir build && cd build
	cmake .. -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DANDROID_NDK="$NDK" -DANDROID_ABI="$ABI" \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_C_FLAGS="-fPIC -fcommon" \
		-DENABLE_OPENSSL=ON \
		-DOPENSSL_ROOT_DIR="$PREFIX" \
		-DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
		-DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" \
		-DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" \
		-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
		-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
		-DLIBSRTP_TEST_APPS=OFF > /dev/null
	make -j"$JOBS" > /dev/null
	make install > /dev/null
	cd "$SRC_DIR"
fi

# ---------------------------------------------------------------- usrsctp
echo "==> usrsctp $USRSCTP_VER"
if [ -f "$PREFIX/lib/libusrsctp.a" ]; then
	echo "    skip, already installed"
else
	if SCTP_DIR=$(reuse_cache usrsctp "$HISI_CACHE/usrsctp-$USRSCTP_VER") && [ -f "$SCTP_DIR/CMakeLists.txt" ]; then
		:
	else
		fetch "https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/$USRSCTP_VER" \
			"usrsctp-$USRSCTP_VER.tar.gz"
		SCTP_DIR=$(unpack "usrsctp-$USRSCTP_VER.tar.gz")
	fi

	cd "$SCTP_DIR"
	rm -rf build && mkdir build && cd build
	cmake .. -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DANDROID_NDK="$NDK" -DANDROID_ABI="$ABI" \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_C_FLAGS="-fPIC -Wno-error -Wno-unused-but-set-variable" \
		-Dsctp_werror=OFF \
		-Dsctp_build_programs=OFF \
		-Dsctp_build_shared_lib=OFF > /dev/null
	make -j"$JOBS" > /dev/null
	make install > /dev/null
	cd "$SRC_DIR"
fi

echo ""
echo "==> opus $OPUS_VER"
if [ -f "$PREFIX/lib/libopus.a" ]; then
	echo "    skip, already installed"
else
	if OPUS_DIR=$(reuse_cache opus "$HISI_CACHE/opus-$OPUS_VER") && [ -f "$OPUS_DIR/configure" ]; then
		:
	elif OPUS_DIR=$(reuse_cache opus "$HISI_CACHE/src-opus" 2>/dev/null) && [ -f "$OPUS_DIR/configure" ]; then
		:
	else
		fetch "https://archive.mozilla.org/pub/opus/opus-${OPUS_VER}.tar.gz" \
			"opus-$OPUS_VER.tar.gz"
		OPUS_DIR=$(unpack "opus-$OPUS_VER.tar.gz")
	fi

	cd "$OPUS_DIR"
	make distclean > /dev/null 2>&1 || true
	CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/${OPUS_HOST}${ANDROID_API}-clang" \
	CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/${OPUS_HOST}${ANDROID_API}-clang++" \
	AR="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar" \
	RANLIB="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib" \
	CFLAGS="-O2 -fPIC" \
	./configure --host="$OPUS_HOST" --prefix="$PREFIX" \
		--disable-shared --enable-static \
		--disable-doc --disable-extra-programs > /dev/null
	make -j"$JOBS" > /dev/null
	make install > /dev/null
	cd "$SRC_DIR"
fi

# ---------------------------------------------------------------- ffmpeg (软解回退用)
echo ""
echo "==> ffmpeg $FFMPEG_VER"
if [ -f "$PREFIX/lib/libavcodec.a" ] && [ -f "$PREFIX/lib/libavutil.a" ]; then
	echo "    skip, already installed"
else
	if FFMPEG_DIR=$(reuse_cache ffmpeg "$HISI_CACHE/ffmpeg-$FFMPEG_VER") && [ -f "$FFMPEG_DIR/configure" ]; then
		:
	else
		# ffmpeg.org 在国内经常超时, 用 GitHub codeload 的 release tag 源码包
		fetch "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n${FFMPEG_VER}" \
			"ffmpeg-$FFMPEG_VER.tar.gz"
		FFMPEG_DIR=$(unpack "ffmpeg-$FFMPEG_VER.tar.gz")
	fi

	export CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/${OPUS_HOST}${ANDROID_API}-clang"
	export CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/${OPUS_HOST}${ANDROID_API}-clang++"
	export AR="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar"
	export STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
	export RANLIB="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib"

	cd "$FFMPEG_DIR"
	make clean > /dev/null 2>&1 || true
	./configure \
		--prefix="$PREFIX" \
		--target-os=android \
		--arch="$FFMPEG_ARCH" \
		--cpu="$FFMPEG_CPU" \
		--cc="$CC" --cxx="$CXX" --ar="$AR" --strip="$STRIP" \
		--enable-cross-compile \
		--enable-static --disable-shared \
		--disable-doc --disable-programs --disable-everything \
		--disable-autodetect --disable-network --disable-avdevice \
		--disable-avfilter --disable-swscale --disable-swresample \
		--disable-postproc --disable-vulkan --disable-vaapi --disable-vdpau \
		--enable-decoder=h264,hevc \
		--enable-parser=h264,hevc \
		--extra-cflags="-fPIC -O2 -DANDROID" \
		--extra-ldflags="-lc" > /dev/null
	make -j"$JOBS" > /dev/null
	make install > /dev/null
	cd "$SRC_DIR"
fi

echo ""
echo "==> done: $PREFIX/lib"
ls -1 "$PREFIX/lib" | sed 's/^/    /'
