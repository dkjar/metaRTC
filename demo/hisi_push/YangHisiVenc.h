//
// 海思 VENC 码流获取(Hi3516CV610, ss_mpi_venc 接口)
//
// 参考:
//   Hi3516CV610/smp/a7_linux/source/mpp/sample/venc_rtsp
//   (sample_comm_venc.c: select(venc_fd) -> query_status -> get_stream -> release_stream)
//   divinus/src/hal/hisi (fd/线程取流结构)
//
// 只消费已存在的 VENC 通道: VI/VPSS/VENC 的初始化由应用(或 SDK sample)完成。
//
#ifndef HISI_PUSH_YANG_HISI_VENC_H
#define HISI_PUSH_YANG_HISI_VENC_H

#include <stdint.h>
#include <atomic>
#include <thread>

#include <yangutil/yangtype.h>

class YangHisiPush;

class YangHisiVenc {
public:
	YangHisiVenc();
	virtual ~YangHisiVenc();

	// chn: VENC 通道号; 从该通道取 Annex-B H.264 码流并交给 push
	int32_t start(int32_t chn, YangHisiPush* push);
	void stop();
	yangbool isRunning();

private:
	void run();

	YangHisiPush* m_push;
	int32_t m_chn;
	std::atomic<bool> m_running;
	std::thread m_thread;
};

#endif
