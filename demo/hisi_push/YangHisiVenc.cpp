//
// 海思 VENC 码流获取(Hi3516CV610, ss_mpi_venc 接口)
//
#include "YangHisiVenc.h"

#include <string.h>
#include <unistd.h>

#include <yangutil/sys/YangLog.h>

#include "YangHisiPush.h"

#ifdef YANG_HISI_MPP

#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <vector>

#include "ss_mpi_venc.h"

#define HISI_VENC_READ_TIMEOUT_MS 200

// 单调时钟(ms), 用于计算"采集到发送"的滞留时间
static int64_t mono_ms() {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 在拼接后的 Annex-B 数据里找 IDR(SPS 也视为关键帧起始)
static yangbool has_h264_key_nal(const uint8_t* data, size_t len) {
	size_t i;

	for (i = 0; i + 4 < len; i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			int32_t type = data[i + 3] & 0x1f;
			if (type == 5 || type == 7)	// IDR / SPS
				return yangtrue;
		}
	}
	return yangfalse;
}

#endif

YangHisiVenc::YangHisiVenc() {
	m_push = NULL;
	m_chn = 0;
	m_running = false;
}

YangHisiVenc::~YangHisiVenc() {
	stop();
}

int32_t YangHisiVenc::start(int32_t chn, YangHisiPush* push) {
#ifndef YANG_HISI_MPP
	(void) chn;
	(void) push;
	yang_error("mpp not enabled, rebuild with Hi3516CV610 SDK: cmake -DHISI_MPP_SDK_DIR=<sdk>/smp/a7_linux/source");
	return 1;
#else
	if (push == NULL || m_running)
		return 1;

	m_chn = chn;
	m_push = push;
	m_running = true;
	m_thread = std::thread(&YangHisiVenc::run, this);
	return Yang_Ok;
#endif
}

void YangHisiVenc::stop() {
	m_running = false;
	if (m_thread.joinable())
		m_thread.join();
}

yangbool YangHisiVenc::isRunning() {
	return m_running;
}

void YangHisiVenc::run() {
#ifdef YANG_HISI_MPP
	td_s32 fd = -1;
	ot_venc_chn_status status;
	ot_venc_stream stream;
	std::vector<ot_venc_pack> packs;
	std::vector<uint8_t> frame;
	int64_t lastPts = -1;
	int32_t idle = 0;
	int32_t sent = 0;

	// 未出帧前每 5 秒打印一次诊断, 区分"没编码输出"和"取流失败"
#define VENC_IDLE_LOG(fmt, ...) \
	do { if (sent == 0 && (++idle % 25 == 0)) yang_error("venc chn %d idle: " fmt, m_chn, ##__VA_ARGS__); } while (0)

	// VENC 通道可能还没创建, 重试等待
	while (m_running && fd < 0) {
		static int32_t wait_cnt = 0;
		fd = ss_mpi_venc_get_fd((ot_venc_chn) m_chn);
		if (fd < 0) {
			if (wait_cnt++ % 10 == 0)
				yang_error("venc chn %d not ready(请确认 VI/VPSS/VENC 已创建并编码), wait ...", m_chn);
			usleep(500 * 1000);
		}
	}
	if (!m_running)
		return;

	yang_error("venc chn %d ready, fd=%d", m_chn, fd);

	// 请求立即产生 IDR, 保证起始帧是带 SPS/PPS 的关键帧
	ss_mpi_venc_request_idr((ot_venc_chn) m_chn, TD_TRUE);

	while (m_running) {
		fd_set read_fds;
		struct timeval tv;

		FD_ZERO(&read_fds);
		FD_SET(fd, &read_fds);
		tv.tv_sec = 0;
		tv.tv_usec = HISI_VENC_READ_TIMEOUT_MS * 1000;

		int32_t ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
		if (ret < 0) {
			yang_error("select venc fd failure");
			break;
		}
		if (ret == 0 || !FD_ISSET(fd, &read_fds)) {
			VENC_IDLE_LOG("select timeout, 编码通道没有可读取流(检查 sample 是否在跑/是否在出帧)");
			continue;
		}

		td_s32 qret = ss_mpi_venc_query_status((ot_venc_chn) m_chn, &status);
		if (qret != TD_SUCCESS) {
			VENC_IDLE_LOG("query_status fail ret=0x%x", qret);
			continue;
		}
		if (status.cur_packs == 0) {
			VENC_IDLE_LOG("cur_packs=0(通道存在但没有编码输出)");
			continue;
		}

		if (packs.size() < status.cur_packs)
			packs.resize(status.cur_packs);

		memset(&stream, 0, sizeof(stream));
		stream.pack = packs.data();
		stream.pack_cnt = status.cur_packs;

		td_s32 gret = ss_mpi_venc_get_stream((ot_venc_chn) m_chn, &stream, HISI_VENC_READ_TIMEOUT_MS);
		if (gret != TD_SUCCESS) {
			VENC_IDLE_LOG("get_stream fail ret=0x%x cur_packs=%d", gret, (int32_t) status.cur_packs);
			usleep(1000);
			continue;
		}

		// 一帧可能由多个 pack 组成, 拼成连续的 Annex-B
		size_t total = 0;
		td_u32 i;
		bool key = false;

		for (i = 0; i < stream.pack_cnt; i++) {
			ot_venc_pack* pack = &stream.pack[i];

			if (pack->data_type.h264_type == OT_VENC_H264_NALU_SEI)
				continue;
			if (pack->addr == NULL || pack->len <= pack->offset)
				continue;
			if (pack->data_type.h264_type == OT_VENC_H264_NALU_IDR_SLICE
					|| pack->data_type.h264_type == OT_VENC_H264_NALU_SPS)
				key = true;

			total += pack->len - pack->offset;
		}

		if (total > 0) {
			size_t off = 0;
			frame.resize(total);
			for (i = 0; i < stream.pack_cnt; i++) {
				ot_venc_pack* pack = &stream.pack[i];
				size_t len;

				if (pack->data_type.h264_type == OT_VENC_H264_NALU_SEI)
					continue;
				if (pack->addr == NULL || pack->len <= pack->offset)
					continue;

				len = pack->len - pack->offset;
				memcpy(frame.data() + off, pack->addr + pack->offset, len);
				off += len;
			}

			if (!key)
				key = has_h264_key_nal(frame.data(), frame.size());

			// metaRTC 的 YangTimestamp 按"微秒 -> 90kHz"换算(*9/100), 官方采集端传的也是微秒,
			// 这里必须保留微秒, 若转成毫秒会导致 RTP 时间戳小 1000 倍, 播放器卡顿/延迟累积
			int64_t pts = (int64_t) stream.pack[0].pts;
			if (pts <= lastPts)
				pts = lastPts + 1;
			lastPts = pts;

			{
				// 调试: 确认 VENC 出帧并送入推流。lag = 采集(MPP pts) -> 送推流 的滞留毫秒数
				static int32_t frame_cnt = 0;
				int64_t lag = mono_ms() - pts / 1000;
				frame_cnt++;
				if (frame_cnt == 1 || frame_cnt % 25 == 0)
					yang_error("venc frame #%d: size=%d key=%d pts=%lld lag=%lldms",
							frame_cnt, (int32_t) frame.size(), key ? 1 : 0, (long long) pts, (long long) lag);
			}

			m_push->sendH264(frame.data(), (int32_t) frame.size(), pts, key ? yangtrue : yangfalse);
			sent++;
		}

		ss_mpi_venc_release_stream((ot_venc_chn) m_chn, &stream);
	}
#endif
}
