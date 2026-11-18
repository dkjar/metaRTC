cmake_minimum_required(VERSION 3.18)
project(yangrtc C CXX)

# 仓库根目录: 优先使用 gradle 传入的 META_RTC_ROOT
if(NOT META_RTC_ROOT)
    get_filename_component(META_RTC_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." REALPATH)
endif()

message(STATUS "META_RTC_ROOT = ${META_RTC_ROOT}")
message(STATUS "ANDROID_ABI   = ${ANDROID_ABI}")

set(YANG_LIB_DIR  "${META_RTC_ROOT}/bin/lib_android/${ANDROID_ABI}")
set(THIRD_LIB_DIR "${META_RTC_ROOT}/thirdparty/lib/android/${ANDROID_ABI}/lib")

if(NOT EXISTS "${YANG_LIB_DIR}/libmetartc8.a")
    message(FATAL_ERROR
        "metaRTC Android 库不存在: ${YANG_LIB_DIR}\n"
        "请先执行 ./thirdparty/build_android_thirdparty.sh 和 ./cmake_lib_android.sh")
endif()
if(NOT EXISTS "${THIRD_LIB_DIR}/libsrtp2.a")
    message(FATAL_ERROR
        "第三方库不存在: ${THIRD_LIB_DIR}\n"
        "请先执行 ./thirdparty/build_android_thirdparty.sh")
endif()

add_library(yangrtc SHARED
    yang_rtc_jni.cpp
    yang_render.cpp
    ${META_RTC_ROOT}/demo/metapushstream8/yangpush/YangRtcPublish.cpp
)

target_include_directories(yangrtc PRIVATE
    ${META_RTC_ROOT}/include
    ${META_RTC_ROOT}/thirdparty/include
    ${META_RTC_ROOT}/thirdparty/user_include
    ${META_RTC_ROOT}/thirdparty/user_include/openssl
    ${META_RTC_ROOT}/thirdparty/user_include/ffmpeg
    ${META_RTC_ROOT}/demo/metapushstream8/yangpush
)

target_compile_definitions(yangrtc PRIVATE __STDC_FORMAT_MACROS)
target_compile_options(yangrtc PRIVATE -fPIC -O2 -Wall -Wno-unused-parameter -std=gnu++11)

target_link_libraries(yangrtc
    ${YANG_LIB_DIR}/libmetartc8.a
    ${YANG_LIB_DIR}/libmetartccore8.a
    ${YANG_LIB_DIR}/libyangwhip8.a
    ${YANG_LIB_DIR}/libyangutil8.a
    ${THIRD_LIB_DIR}/libsrtp2.a
    ${THIRD_LIB_DIR}/libssl.a
    ${THIRD_LIB_DIR}/libcrypto.a
    ${THIRD_LIB_DIR}/libusrsctp.a
    ${THIRD_LIB_DIR}/libopus.a
    # FFmpeg 软解 (videoDecHwType==0 或硬解失败回退)
    ${THIRD_LIB_DIR}/libavcodec.a
    ${THIRD_LIB_DIR}/libavutil.a
    android
    mediandk
    log
    EGL
    GLESv3
    z
    dl
    m
)
