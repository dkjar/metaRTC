//
// 海思(HiSilicon)音视频 -> SRS(WHIP) 推流封装
//
#include "YangHisiPush.h"

#include <string.h>

#include <yangutil/yangavinfotype.h>
#include <yangutil/sys/YangLog.h>
#include <yangutil/sys/YangMath.h>
#include <yangutil/sys/YangThread2.h>
#include <yangutil/buffer/YangVideoEncoderBuffer.h>
#include <yangpush/YangRtcPublish.h>

YangHisiPush::YangHisiPush() {
	m_context = NULL;
	m_pub = NULL;
	m_vbuf = NULL;
	m_started = yangfalse;
}

YangHisiPush::~YangHisiPush() {
	stop();
	yang_delete(m_vbuf);
	yang_delete(m_context);
}

int32_t YangHisiPush::init(const char* url) {
	int32_t err = Yang_Ok;

	if (url == NULL)
		return 1;

	m_context = new YangContext();
	m_context->init();

	m_context->avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
	m_context->avinfo.sys.logLevel = 2;

	m_context->avinfo.video.videoEncoderType = Yang_VED_H264;

	m_context->avinfo.audio.audioEncoderType = Yang_AED_OPUS;
	m_context->avinfo.audio.sample = 48000;
	m_context->avinfo.audio.enableAudioFec = yangfalse;	// SRS 不支持音频 FEC

	m_context->avinfo.rtc.iceCandidateType = YangIceHost;
	m_context->avinfo.rtc.rtcLocalPort = 10000 + yang_random() % 15000;

	yang_setLogLevel(m_context->avinfo.sys.logLevel);

	// 视频队列深度直接决定最坏情况下的排队延迟(4 帧 ≈ 133ms @30fps), 默认 10 偏大
	m_context->avinfo.video.evideoCacheNum = 4;

	m_vbuf = new YangVideoEncoderBuffer(m_context->avinfo.video.evideoCacheNum);
	m_pub = new YangRtcPublish(m_context);
	m_pub->setInVideoList(m_vbuf);

	err = m_pub->init((char*) url, yangtrue);
	if (err) {
		yang_error("connect srs(%s) failure, err=%d", url, err);
		return err;
	}
	return Yang_Ok;
}

int32_t YangHisiPush::start() {
	if (m_pub == NULL)
		return 1;
	if (m_started)
		return Yang_Ok;

	m_pub->start();
	m_started = yangtrue;
	return Yang_Ok;
}

yangbool YangHisiPush::isPublished() {
	return (m_pub && m_pub->isPublished) ? yangtrue : yangfalse;
}

int32_t YangHisiPush::sendH264(const uint8_t* data, int32_t len, int64_t ptsMs, yangbool keyframe) {
	YangFrame frame;

	if (m_vbuf == NULL || data == NULL || len <= 0)
		return 1;

	memset(&frame, 0, sizeof(frame));
	frame.mediaType = YangFrameTypeVideo;
	frame.frametype = keyframe ? YANG_Frametype_I : YANG_Frametype_P;
	frame.payload = (uint8_t*) data;
	frame.nb = len;
	frame.pts = ptsMs;

	m_vbuf->putEVideo(&frame);
	return Yang_Ok;
}

void YangHisiPush::stop() {
	if (m_pub) {
		m_pub->stop();
		yang_stop_thread(m_pub);
		yang_delete(m_pub);
	}
	m_started = yangfalse;
}
