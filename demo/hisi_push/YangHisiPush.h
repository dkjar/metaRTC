//
// 海思(HiSilicon)音视频 -> SRS(WHIP) 推流封装
//
// 只推视频：把海思 VENC 输出的 Annex-B H.264 帧交给 sendH264()。
// 前提：libmetartc8 以 NoCapture=ON 编译（不含采集/编码），本类只做 RTP/WHIP 打包发送。
//
#ifndef HISI_PUSH_YANG_HISI_PUSH_H
#define HISI_PUSH_YANG_HISI_PUSH_H

#include <stdint.h>
#include <yangutil/yangtype.h>

class YangContext;
class YangRtcPublish;
class YangVideoEncoderBuffer;

class YangHisiPush {
public:
	YangHisiPush();
	virtual ~YangHisiPush();

	// url: http://<srs_ip>:1985/rtc/v1/whip/?app=live&stream=livestream
	int32_t init(const char* url);

	// 启动发送线程，内部完成 WHIP 的 SDP 交换
	int32_t start();

	// 是否已连上 SRS（可以开始送帧）
	yangbool isPublished();

	// 海思 VENC 每出一帧调用一次
	// data: 完整 Annex-B 数据(00 00 00 01 起始码)
	// keyframe: true 时必须包含 SPS/PPS 的 I 帧
	// ptsMs: 毫秒
	int32_t sendH264(const uint8_t* data, int32_t len, int64_t ptsMs, yangbool keyframe);

	void stop();

private:
	YangContext* m_context;
	YangRtcPublish* m_pub;
	YangVideoEncoderBuffer* m_vbuf;
	yangbool m_started;
};

#endif
