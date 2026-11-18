cmake_minimum_required(VERSION 3.10)
project(hisi_push C CXX)

set(HOME_BASE_REL "../../")
get_filename_component(HOME_BASE ${HOME_BASE_REL} REALPATH)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(YANG_LIB_DIR ${HOME_BASE}/bin/lib_debug CACHE PATH "metaRTC static lib dir")
set(THIRD_LIB_DIR ${HOME_BASE}/thirdparty/lib/hisi/lib CACHE PATH "thirdparty static lib dir (arm musl)")

if(NOT EXISTS ${YANG_LIB_DIR}/libmetartc8.a)
    message(WARNING "metaRTC libs not found in ${YANG_LIB_DIR}, run ../../cmake_lib_hisi.sh first")
endif()
if(NOT EXISTS ${THIRD_LIB_DIR})
    message(WARNING "thirdparty libs not found in ${THIRD_LIB_DIR}, cross compile openssl/libsrtp2/usrsctp/opus first")
endif()

add_executable(hisi_push
    main.cpp
    YangHisiPush.cpp
    YangHisiVenc.cpp
    ${HOME_BASE}/demo/metapushstream8/yangpush/YangRtcPublish.cpp
)

target_include_directories(hisi_push PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HOME_BASE}/include
)

target_link_directories(hisi_push PRIVATE
    ${YANG_LIB_DIR}
    ${THIRD_LIB_DIR}
)

# 板端 rootfs 一般没有 libstdc++, 静态链入, 只保留 libc(musl) 依赖
target_link_options(hisi_push PRIVATE -static-libstdc++ -static-libgcc)

target_link_libraries(hisi_push PRIVATE
    metartc8
    metartccore8
    yangwhip8
    yangutil8
    srtp2
    ssl
    crypto
    usrsctp
    pthread
    dl
    m
)

# 海思 MPP: 有 SDK 才编入 VENC 取流(ss_mpi_venc)
set(HISI_MPP_SDK_DIR "${HOME_BASE}/../Hi3516CV610/smp/a7_linux/source" CACHE PATH "Hi3516CV610 SDK source dir")
option(HISI_PIPELINE "hisi_push 内部初始化 VI/VPSS/VENC, 不再依赖 sample_venc_rtsp" ON)
if(EXISTS ${HISI_MPP_SDK_DIR}/out/include/ss_mpi_venc.h)
    message(STATUS "MPP enabled: ${HISI_MPP_SDK_DIR}")
    target_compile_definitions(hisi_push PRIVATE YANG_HISI_MPP)
    target_include_directories(hisi_push PRIVATE ${HISI_MPP_SDK_DIR}/out/include)

    set(SAMPLE_DIR ${HISI_MPP_SDK_DIR}/mpp/sample)
    if(HISI_PIPELINE AND EXISTS ${SAMPLE_DIR}/common/sample_comm.h)
        message(STATUS "pipeline enabled: build sample_comm + venc_rtsp(as lib) into hisi_push")
        file(GLOB SAMPLE_COMM_SRCS ${SAMPLE_DIR}/common/*.c)
        add_library(hisi_pipeline STATIC
            ${SAMPLE_COMM_SRCS}
            ${SAMPLE_DIR}/venc_rtsp/sample_venc_rtsp.c
        )
        target_compile_definitions(hisi_pipeline PRIVATE
            SAMPLE_VENC_AS_LIB
            _LINUX hi3516cv610 OT_XXXX ISP_V2 OT_RELEASE
            USER_BIT_32 KERNEL_BIT_32
            SENSOR0_TYPE=SC4336P_MIPI_4M_30FPS_10BIT
            SENSOR1_TYPE=SC4336P_MIPI_4M_30FPS_10BIT
            SC4336P_MIPI_4M_30FPS_10BIT_SELECT=y
            BOARD_TYPE=DMEB_QFN OT_ACODEC_TYPE_INNER OT_VQE_USE_STATIC_MODULE_REGISTER
        )
        target_compile_options(hisi_pipeline PRIVATE -fsigned-char -fPIC -O2 -Wno-unused-function)
        target_include_directories(hisi_pipeline PRIVATE
            ${SAMPLE_DIR}/common
            ${SAMPLE_DIR}/audio/adp
            ${HISI_MPP_SDK_DIR}/out/include
            ${HISI_MPP_SDK_DIR}/out/include/exp_inc
            ${HISI_MPP_SDK_DIR}/osal/include
            ${HISI_MPP_SDK_DIR}/../platform/securec/include
            ${HISI_MPP_SDK_DIR}/../../vendor/extdrv/es8388
        )
        target_compile_definitions(hisi_push PRIVATE YANG_HISI_PIPELINE)
        target_link_libraries(hisi_push PRIVATE hisi_pipeline)

        # 与 sample 相同的 MPP 静态库清单(全路径 .a, 避免链到 .so)
        target_link_libraries(hisi_push PRIVATE
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_sysbind.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_sysmem.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_ae.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_isp.a
            ${HISI_MPP_SDK_DIR}/out/lib/libot_mpi_isp.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_awb.a
            ${HISI_MPP_SDK_DIR}/out/lib/libdehaze.a
            ${HISI_MPP_SDK_DIR}/out/lib/libextend_stats.a
            ${HISI_MPP_SDK_DIR}/out/lib/libdrc.a
            ${HISI_MPP_SDK_DIR}/out/lib/libldci.a
            ${HISI_MPP_SDK_DIR}/out/lib/libbnr.a
            ${HISI_MPP_SDK_DIR}/out/lib/libcalcflicker.a
            ${HISI_MPP_SDK_DIR}/out/lib/libir_auto.a
            ${HISI_MPP_SDK_DIR}/out/lib/libacs.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_sc4336p.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_gc4023.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_sc431hai.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_sc450ai.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_sc500ai.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsns_os04d10.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_aibnr.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsvp_acl.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_km.a
            ${HISI_MPP_SDK_DIR}/out/lib/libprotobuf-c.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_audio.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvoice_engine.a
            ${HISI_MPP_SDK_DIR}/out/lib/libupvqe.a
            ${HISI_MPP_SDK_DIR}/out/lib/libdnvqe.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_audio_adp.a
            ${HISI_MPP_SDK_DIR}/out/lib/libopus.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_res.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_record.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_hpf.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_anr.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_aec.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_agc.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_eq.a
            ${HISI_MPP_SDK_DIR}/out/lib/libvqe_talkv2.a
            ${HISI_MPP_SDK_DIR}/out/lib/libaac_dec.a
            ${HISI_MPP_SDK_DIR}/out/lib/libaac_enc.a
            ${HISI_MPP_SDK_DIR}/out/lib/libaac_comm.a
            ${HISI_MPP_SDK_DIR}/out/lib/libaac_sbr_dec.a
            ${HISI_MPP_SDK_DIR}/out/lib/libaac_sbr_enc.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsecurec.a
        )
    else()
        # 只取流(外部应用已建好编码通道)
        target_link_libraries(hisi_push PRIVATE
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_sysbind.a
            ${HISI_MPP_SDK_DIR}/out/lib/libss_mpi_sysmem.a
            ${HISI_MPP_SDK_DIR}/out/lib/libsecurec.a
        )
    endif()
else()
    message(WARNING "MPP SDK not found in ${HISI_MPP_SDK_DIR}, venc_chn will be unavailable")
endif()
