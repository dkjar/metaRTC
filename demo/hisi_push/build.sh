#!/bin/bash
# 交叉编译海思(arm-linux-musleabi)推流示例
# 前置：在仓库根目录执行过 ./cmake_lib_hisi.sh，且 thirdparty/lib/hisi 已有 openssl/libsrtp2/usrsctp
set -e

cd "$(dirname "$0")"

rm -rf build
mkdir build
cd build

cmake -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE=../../arch/hisi_arm32.cmake \
	..

make -j8

echo "output: $(pwd)/hisi_push"
