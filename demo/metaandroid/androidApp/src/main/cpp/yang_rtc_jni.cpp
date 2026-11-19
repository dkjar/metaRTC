//
// metaRTC Android JNI 桥接
//   推流: Kotlin(CameraX + MediaCodec) 产出 H.264 Annex-B -> sendH264Frame -> WHIP
//   拉流: WHEP 收流 -> MediaCodec(NDK) 硬解 -> 直出 Surface
//         (硬解不可用时回退 FFmpeg 软解 -> I420 -> ANativeWindow)
//
// 变更备注:
//   2026-11-16  解码从收包线程移到独立线程(4 帧有界队列, 满则丢最旧);
//               硬解改为 Surface 直出; RTP 90kHz 时间戳转微秒后再给 MediaCodec
//   2026-11-19  增加 rx/dec/drop 每 5 秒统计日志, 用于定位拉流卡顿;
//               队列溢出改为「丢到下一个关键帧 + 请求 PLI」, 修复快速运动花屏;
//               PLI 改为解码线程发送(收包回调里直接发会 SIGSEGV), 并加 recv 生命周期锁
//
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <yangutil/yangavinfotype.h>
#include <yangutil/sys/YangLog.h>
#include <yangutil/sys/YangMath.h>
#include <yangutil/sys/YangThread2.h>
#include <yangutil/buffer/YangVideoEncoderBuffer.h>
#include <yangpush/YangRtcPublish.h>
#include <yangrtc/YangWhip.h>
#include <yangrtc/YangPeerInfo.h>
#include <yangdecoder/YangDecoderFactory.h>
#include <yangdecoder/YangVideoDecoder.h>

#include "yang_render.h"

#define LOG_TAG "YangRtcJni"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------- 结构

class YangRenderCallback: public YangDecoderCallback {
public:
	YangAndroidRender *render;
	int32_t width;
	int32_t height;

	YangRenderCallback() {
		render = NULL;
		width = 0;
		height = 0;
	}

	void onAudioData(YangFrame *pframe) {
		(void) pframe;
	}

	void onVideoData(YangFrame *pframe) {
		if (render == NULL || pframe == NULL || pframe->payload == NULL)
			return;
		if (width <= 0 || height <= 0)
			return;
		render->drawI420(width, height, pframe->payload, pframe->nb);
	}
};

struct YangAndroidRtc {
	// 推流
	YangContext *pubCtx;
	YangVideoEncoderBuffer *vbuf;
	YangRtcPublish *pub;
	// 拉流
	YangContext *playCtx;
	YangPeerConnection *recv;
	YangVideoDecoder *dec;
	YangRenderCallback cb;
	YangAndroidRender *render;
	ANativeWindow *win;
	pthread_t playThread;
	volatile int32_t playing;
	volatile int32_t playStarted;
	char *playUrl;

	// 解码线程: 收包线程只做入队, 解码+渲染在这里做, 避免阻塞收包
	pthread_t decThread;
	volatile int32_t decRunning;
	volatile int32_t decStarted;
	pthread_mutex_t decLock;	// 保护 dec 指针
	pthread_mutex_t qLock;
	pthread_cond_t qCond;
	struct VideoPacket {
		uint8_t *data;
		int32_t nb;
		int64_t ptsUs;
		int32_t keyFrame;
	};
	VideoPacket qPkts[4];
	int32_t qHead;
	int32_t qTail;
	int32_t qCount;
	// 队列溢出后进入「丢弃直到下一个关键帧」模式:
	// H.264 是帧间预测, 丢任意 P 帧会让后续帧失去参考 -> 花屏,
	// 所以宁可短暂停顿, 也不能丢中间帧
	volatile int32_t skipUntilKey;

	// 统计: 收到的帧 / 解码的帧 / 队列丢弃的帧
	// 备注(2026-11-19): 每 5 秒打印一次, 用于区分「收流丢包 / 解码慢 / 队列溢出」
	volatile int64_t statRx;
	volatile int64_t statDec;
	volatile int64_t statDrop;
	volatile int64_t lastPliMs;	// PLI 限频
	// 备注(2026-11-19): PLI 不能在收包回调里同步发送(会重入 RTC 栈导致 SIGSEGV),
	// 只置标志由解码线程发送; recvLock 保护 recv 指针的生命周期
	pthread_mutex_t recvLock;
	volatile int32_t pliPending;

	YangAndroidRtc() {
		pubCtx = NULL;
		vbuf = NULL;
		pub = NULL;
		playCtx = NULL;
		recv = NULL;
		dec = NULL;
		render = new YangAndroidRender();
		win = NULL;
		playing = 0;
		playStarted = 0;
		playUrl = NULL;

		decRunning = 0;
		decStarted = 0;
		skipUntilKey = 0;
		statRx = 0;
		statDec = 0;
		statDrop = 0;
		lastPliMs = 0;
		pliPending = 0;
		qHead = qTail = qCount = 0;
		memset(qPkts, 0, sizeof(qPkts));
		pthread_mutex_init(&recvLock, NULL);
		pthread_mutex_init(&decLock, NULL);
		pthread_mutex_init(&qLock, NULL);
		pthread_cond_init(&qCond, NULL);
	}

	~YangAndroidRtc() {
		stopPublish();
		stopPlay();
		if (render) {
			delete render;
			render = NULL;
		}
		pthread_mutex_destroy(&recvLock);
		pthread_mutex_destroy(&decLock);
		pthread_mutex_destroy(&qLock);
		pthread_cond_destroy(&qCond);
	}

	void stopPublish();
	void stopPlay();
};

// ---------------------------------------------------------------- 推流

static int32_t publishInit(YangAndroidRtc *rtc, const char *url) {
	if (rtc->pub != NULL)
		rtc->stopPublish();

	rtc->pubCtx = new YangContext();
	rtc->pubCtx->init();

	rtc->pubCtx->avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
	rtc->pubCtx->avinfo.sys.logLevel = 3;
	rtc->pubCtx->avinfo.video.videoEncoderType = Yang_VED_H264;
	rtc->pubCtx->avinfo.rtc.iceCandidateType = YangIceHost;
	rtc->pubCtx->avinfo.rtc.rtcLocalPort = 10000 + yang_random() % 15000;
	// 队列深度直接决定最坏排队延迟, 4 帧约 133ms
	rtc->pubCtx->avinfo.video.evideoCacheNum = 4;
	yang_setLogLevel(rtc->pubCtx->avinfo.sys.logLevel);

	rtc->vbuf = new YangVideoEncoderBuffer(rtc->pubCtx->avinfo.video.evideoCacheNum);
	rtc->pub = new YangRtcPublish(rtc->pubCtx);
	rtc->pub->setInVideoList(rtc->vbuf);

	int32_t err = rtc->pub->init((char *) url, yangtrue);
	if (err) {
		ALOGE("publish init failure, url=%s err=%d", url, err);
		rtc->stopPublish();
		return err;
	}

	rtc->pub->start();
	ALOGI("publish started: %s", url);
	return Yang_Ok;
}

void YangAndroidRtc::stopPublish() {
	if (pub != NULL) {
		pub->stop();
		yang_stop_thread(pub);
		delete pub;
		pub = NULL;
	}
	if (vbuf != NULL) {
		delete vbuf;
		vbuf = NULL;
	}
	if (pubCtx != NULL) {
		delete pubCtx;
		pubCtx = NULL;
	}
}

// ---------------------------------------------------------------- 拉流

#define VIDEO_QUEUE_SIZE 4

// 调用方需持有 qLock
static void queueClearLocked(YangAndroidRtc *rtc) {
	for (int32_t i = 0; i < rtc->qCount; i++) {
		int32_t idx = (rtc->qHead + i) % VIDEO_QUEUE_SIZE;
		free(rtc->qPkts[idx].data);
		rtc->qPkts[idx].data = NULL;
	}
	rtc->qHead = rtc->qTail = rtc->qCount = 0;
}

static void queueClear(YangAndroidRtc *rtc) {
	pthread_mutex_lock(&rtc->qLock);
	queueClearLocked(rtc);
	pthread_mutex_unlock(&rtc->qLock);
}

// 请求关键帧(PLI)。
// 注意: 该函数会被收包回调调用, 绝不能在回调里直接发 RTCP ——
// 会重入 RTC 栈(实测 SIGSEGV), 这里只置标志, 由解码线程真正发送。
static void requestKeyFrame(YangAndroidRtc *rtc) {
	rtc->pliPending = 1;
}

// 由解码线程调用: 限频 1 秒真正发出 PLI
static void flushKeyFrameRequest(YangAndroidRtc *rtc) {
	struct timespec ts;
	int64_t nowMs;

	if (!rtc->pliPending)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	nowMs = (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	if (rtc->lastPliMs != 0 && nowMs - rtc->lastPliMs < 1000)
		return;	// 保留 pending, 稍后再发

	rtc->lastPliMs = nowMs;
	rtc->pliPending = 0;

	pthread_mutex_lock(&rtc->recvLock);
	if (rtc->recv != NULL && rtc->recv->sendRequestPli != NULL)
		rtc->recv->sendRequestPli(&rtc->recv->peer);
	pthread_mutex_unlock(&rtc->recvLock);
}

// 入队。溢出时不再丢单个 P 帧(会破坏参考链导致花屏),
// 而是清空队列并丢到下一个关键帧为止, 同时请求 PLI
static void queuePush(YangAndroidRtc *rtc, const uint8_t *data, int32_t nb,
		int64_t ptsUs, int32_t keyFrame) {
	if (data == NULL || nb <= 0)
		return;

	uint8_t *copy = (uint8_t *) malloc(nb);
	if (copy == NULL)
		return;
	memcpy(copy, data, nb);

	pthread_mutex_lock(&rtc->qLock);

	// 跳过模式: 只接收关键帧
	if (rtc->skipUntilKey) {
		if (!keyFrame) {
			free(copy);
			rtc->statDrop++;
			pthread_mutex_unlock(&rtc->qLock);
			return;
		}
		queueClearLocked(rtc);
		rtc->skipUntilKey = 0;
	}

	if (rtc->qCount == VIDEO_QUEUE_SIZE) {
		int32_t dropped = rtc->qCount;
		queueClearLocked(rtc);
		rtc->statDrop += dropped;

		if (keyFrame) {
			// 溢出的正好是关键帧, 清空后直接用它重新开始
			rtc->skipUntilKey = 0;
		} else {
			// 丢掉当前 P 帧和后续所有 P 帧, 等下一个 IDR
			free(copy);
			rtc->statDrop++;
			rtc->skipUntilKey = 1;
			pthread_mutex_unlock(&rtc->qLock);
			ALOGW("decoder queue overflow, skip to next keyframe");
			requestKeyFrame(rtc);
			return;
		}
	}

	rtc->qPkts[rtc->qTail].data = copy;
	rtc->qPkts[rtc->qTail].nb = nb;
	rtc->qPkts[rtc->qTail].ptsUs = ptsUs;
	rtc->qPkts[rtc->qTail].keyFrame = keyFrame;
	rtc->qTail = (rtc->qTail + 1) % VIDEO_QUEUE_SIZE;
	rtc->qCount++;
	pthread_cond_signal(&rtc->qCond);
	pthread_mutex_unlock(&rtc->qLock);
}

static int32_t queuePop(YangAndroidRtc *rtc, YangAndroidRtc::VideoPacket *out) {
	pthread_mutex_lock(&rtc->qLock);
	if (rtc->qCount == 0 && rtc->decRunning) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&rtc->qCond, &rtc->qLock, &ts);
	}
	if (rtc->qCount == 0) {
		pthread_mutex_unlock(&rtc->qLock);
		return 0;
	}
	*out = rtc->qPkts[rtc->qHead];
	rtc->qPkts[rtc->qHead].data = NULL;
	rtc->qHead = (rtc->qHead + 1) % VIDEO_QUEUE_SIZE;
	rtc->qCount--;
	pthread_mutex_unlock(&rtc->qLock);
	return 1;
}

static void *decodeThreadRun(void *arg) {
	YangAndroidRtc *rtc = (YangAndroidRtc *) arg;
	YangFrame frame;
	int64_t lastRx = 0, lastDec = 0, lastDrop = 0;
	struct timespec lastTs, nowTs;
	clock_gettime(CLOCK_MONOTONIC, &lastTs);

	while (rtc->decRunning) {
		YangAndroidRtc::VideoPacket pkt;
		if (queuePop(rtc, &pkt)) {
			pthread_mutex_lock(&rtc->decLock);
			YangVideoDecoder *dec = rtc->dec;
			if (dec != NULL) {
				memset(&frame, 0, sizeof(frame));
				frame.mediaType = YangFrameTypeVideo;
				frame.frametype = pkt.keyFrame ? YANG_Frametype_I : YANG_Frametype_P;
				frame.payload = pkt.data;
				frame.nb = pkt.nb;
				frame.pts = pkt.ptsUs;
				if (dec->decode(&frame, YangI420, &rtc->cb) == 2) {
					// 解码器持续取不到输出, 请求关键帧重新同步
					requestKeyFrame(rtc);
				}
				rtc->statDec++;
			}
			pthread_mutex_unlock(&rtc->decLock);
			free(pkt.data);
		}

		// 需要补关键帧时, 由本线程(而不是收包线程)发送 PLI
		flushKeyFrameRequest(rtc);

		clock_gettime(CLOCK_MONOTONIC, &nowTs);
		int64_t elapsedMs = (nowTs.tv_sec - lastTs.tv_sec) * 1000
				+ (nowTs.tv_nsec - lastTs.tv_nsec) / 1000000;
		if (elapsedMs >= 5000) {
			int64_t rx = rtc->statRx, dc = rtc->statDec, dp = rtc->statDrop;
			ALOGI("play stats: rx=%lld dec=%lld drop=%lld in %lldms",
					(long long) (rx - lastRx), (long long) (dc - lastDec),
					(long long) (dp - lastDrop), (long long) elapsedMs);
			lastRx = rx;
			lastDec = dc;
			lastDrop = dp;
			lastTs = nowTs;
		}
	}
	return NULL;
}

static void onRecvVideo(void *user, YangFrame *frame);
static void onRecvAudio(void *user, YangFrame *frame) {
	(void) user;
	(void) frame;
}
static void onRecvMsg(void *user, YangFrame *frame) {
	(void) user;
	(void) frame;
}

// 判断 Annex-B 里是否有 SPS(nal type 7)
static int32_t hasSps(const uint8_t *data, int32_t len) {
	int32_t i = 0;
	while (i + 4 < len) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			return ((data[i + 3] & 0x1f) == 7) ? 1 : 0;
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
			return ((data[i + 4] & 0x1f) == 7) ? 1 : 0;
		i++;
	}
	return 0;
}

static void onRecvVideo(void *user, YangFrame *frame) {
	YangAndroidRtc *rtc = (YangAndroidRtc *) user;

	if (rtc == NULL || frame == NULL || frame->payload == NULL || frame->nb <= 0)
		return;

	pthread_mutex_lock(&rtc->decLock);
	YangVideoDecoder *dec = rtc->dec;
	if (dec == NULL) {
		pthread_mutex_unlock(&rtc->decLock);
		return;
	}

	// 收到 SPS 时解析分辨率, 同时初始化解码器
	if (rtc->cb.width <= 0 && hasSps(frame->payload, frame->nb)) {
		int32_t w = 0, h = 0, fps = 0;
		dec->parseHeader(frame->payload, frame->nb, &w, &h, &fps);

		// 硬解初始化失败 (MediaCodec configure/start 失败) -> 回退 FFmpeg 软解
		if (dec->m_isInit == 0 && rtc->playCtx->avinfo.video.videoDecHwType != 0) {
			ALOGE("hardware decoder init failed, fallback to FFmpeg software decode");
			delete dec;
			rtc->playCtx->avinfo.video.videoDecHwType = 0;
			YangDecoderFactory factory;
			dec = factory.createVideoDecoder(Yang_VED_H264, &rtc->playCtx->avinfo.video);
			if (dec != NULL) {
				dec->init();
				dec->parseHeader(frame->payload, frame->nb, &w, &h, &fps);
			}
			rtc->dec = dec;
		}

		if (w > 0 && h > 0) {
			rtc->cb.width = w;
			rtc->cb.height = h;
			ALOGI("remote video %dx%d @%d", w, h, fps);
		}
	}
	pthread_mutex_unlock(&rtc->decLock);

	// RTP 时间戳是 90kHz, 转成 MediaCodec 需要的微秒
	int64_t ptsUs = frame->pts * 1000 / 90;
	rtc->statRx++;
	queuePush(rtc, frame->payload, frame->nb, ptsUs,
			frame->frametype == YANG_Frametype_I ? 1 : 0);
}

static void *playThreadRun(void *arg) {
	YangAndroidRtc *rtc = (YangAndroidRtc *) arg;
	int32_t err;

	YangDecoderFactory factory;
	YangVideoDecoder *dec = factory.createVideoDecoder(Yang_VED_H264,
			&rtc->playCtx->avinfo.video);
	if (dec != NULL) {
		dec->init();
		// 硬解直接输出到 Surface, 不做 CPU 颜色转换
		if (rtc->playCtx->avinfo.video.videoDecHwType != 0)
			dec->setRenderWindow(rtc->win);
	}

	pthread_mutex_lock(&rtc->decLock);
	rtc->dec = dec;
	pthread_mutex_unlock(&rtc->decLock);

	ALOGI("decoder=%s", rtc->playCtx->avinfo.video.videoDecHwType != 0
			? "mediacodec(surface)" : "ffmpeg(soft)");

	// 启动独立解码线程
	rtc->decRunning = 1;
	if (pthread_create(&rtc->decThread, NULL, decodeThreadRun, rtc) == 0)
		rtc->decStarted = 1;
	else
		rtc->decRunning = 0;

	err = yang_whip_connectWhipWhepServer(&rtc->recv->peer, rtc->playUrl);
	if (err)
		ALOGE("whep connect failure, err=%d", err);
	else
		ALOGI("whep connected: %s", rtc->playUrl);

	while (rtc->playing)
		usleep(100 * 1000);

	// 先停解码线程(它可能正在发 PLI), 再关连接, 避免关闭过程中被并发访问
	rtc->decRunning = 0;
	pthread_mutex_lock(&rtc->qLock);
	pthread_cond_broadcast(&rtc->qCond);
	pthread_mutex_unlock(&rtc->qLock);
	if (rtc->decStarted) {
		pthread_join(rtc->decThread, NULL);
		rtc->decStarted = 0;
	}
	queueClear(rtc);

	pthread_mutex_lock(&rtc->recvLock);
	if (rtc->recv != NULL)
		rtc->recv->close(&rtc->recv->peer);
	pthread_mutex_unlock(&rtc->recvLock);

	pthread_mutex_lock(&rtc->decLock);
	dec = rtc->dec;
	rtc->dec = NULL;
	pthread_mutex_unlock(&rtc->decLock);
	if (dec != NULL)
		delete dec;

	ALOGI("play thread exit");
	return NULL;
}

static int32_t playStart(YangAndroidRtc *rtc, const char *url, ANativeWindow *win, int32_t hwDecode) {
	int32_t len;

	if (rtc->playing)
		rtc->stopPlay();

	if (win == NULL)
		return 1;

	rtc->render->setWindow(win);
	rtc->win = win;
	rtc->cb.render = rtc->render;
	rtc->skipUntilKey = 0;
	rtc->cb.width = 0;
	rtc->cb.height = 0;

	rtc->playCtx = new YangContext();
	rtc->playCtx->init();
	rtc->playCtx->avinfo.sys.mediaServer = Yang_Server_Whip_Whep;
	rtc->playCtx->avinfo.sys.logLevel = 3;
	rtc->playCtx->avinfo.rtc.iceCandidateType = YangIceHost;
	rtc->playCtx->avinfo.rtc.rtcLocalPort = 20000 + yang_random() % 15000;
	// 1=MediaCodec 硬解(失败自动回退 FFmpeg 软解), 0=FFmpeg 软解
	rtc->playCtx->avinfo.video.videoDecHwType = hwDecode ? 1 : 0;
	yang_setLogLevel(rtc->playCtx->avinfo.sys.logLevel);

	len = (int32_t) strlen(url);
	rtc->playUrl = (char *) malloc(len + 1);
	memcpy(rtc->playUrl, url, len);
	rtc->playUrl[len] = 0;

	rtc->recv = (YangPeerConnection *) calloc(1, sizeof(YangPeerConnection));
	yang_avinfo_initPeerInfo(&rtc->recv->peer.peerInfo, &rtc->playCtx->avinfo);
	rtc->recv->peer.peerInfo.rtc.rtcLocalPort = rtc->playCtx->avinfo.rtc.rtcLocalPort++;
	rtc->recv->peer.peerInfo.direction = YangRecvonly;
	rtc->recv->peer.peerInfo.uid = 0;

	rtc->recv->peer.peerCallback.recvCallback.context = rtc;
	rtc->recv->peer.peerCallback.recvCallback.receiveVideo = onRecvVideo;
	rtc->recv->peer.peerCallback.recvCallback.receiveAudio = onRecvAudio;
	rtc->recv->peer.peerCallback.recvCallback.receiveMsg = onRecvMsg;
	memcpy(&rtc->recv->peer.peerCallback.rtcCallback, &rtc->playCtx->rtcCallback,
			sizeof(YangRtcCallback));

	yang_create_peerConnection(rtc->recv);
	rtc->recv->addVideoTrack(&rtc->recv->peer, Yang_VED_H264);
	rtc->recv->addTransceiver(&rtc->recv->peer, YangMediaVideo, YangRecvonly);

	rtc->playing = 1;
	rtc->playStarted = 1;
	if (pthread_create(&rtc->playThread, NULL, playThreadRun, rtc) != 0) {
		rtc->playStarted = 0;
		rtc->playing = 0;
		ALOGE("create play thread failure");
		return 1;
	}

	ALOGI("play started: %s", url);
	return Yang_Ok;
}

void YangAndroidRtc::stopPlay() {
	if (playing) {
		playing = 0;
		if (playStarted)
			pthread_join(playThread, NULL);
	}

	if (recv != NULL) {
		pthread_mutex_lock(&recvLock);
		free(recv);
		recv = NULL;
		pthread_mutex_unlock(&recvLock);
	}
	if (playCtx != NULL) {
		delete playCtx;
		playCtx = NULL;
	}
	if (playUrl != NULL) {
		free(playUrl);
		playUrl = NULL;
	}
	render->setWindow(NULL);
	win = NULL;
	cb.width = 0;
	cb.height = 0;
}

// ---------------------------------------------------------------- JNI

static YangAndroidRtc *asRtc(jlong handle) {
	return (YangAndroidRtc *) (intptr_t) handle;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_meta_rtc_YangRtcNative_create(JNIEnv *env, jclass clazz) {
	(void) env;
	(void) clazz;
	return (jlong) (intptr_t) new YangAndroidRtc();
}

extern "C" JNIEXPORT void JNICALL
Java_org_meta_rtc_YangRtcNative_destroy(JNIEnv *env, jclass clazz, jlong handle) {
	(void) env;
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	if (rtc != NULL)
		delete rtc;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_meta_rtc_YangRtcNative_startPublish(JNIEnv *env, jclass clazz, jlong handle,
		jstring url) {
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	const char *curl;

	if (rtc == NULL || url == NULL)
		return 1;

	curl = env->GetStringUTFChars(url, NULL);
	if (curl == NULL)
		return 1;
	int32_t err = publishInit(rtc, curl);
	env->ReleaseStringUTFChars(url, curl);
	return err;
}

extern "C" JNIEXPORT void JNICALL
Java_org_meta_rtc_YangRtcNative_stopPublish(JNIEnv *env, jclass clazz, jlong handle) {
	(void) env;
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	if (rtc != NULL)
		rtc->stopPublish();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_meta_rtc_YangRtcNative_isPublishing(JNIEnv *env, jclass clazz, jlong handle) {
	(void) env;
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	if (rtc == NULL || rtc->pub == NULL)
		return JNI_FALSE;
	return rtc->pub->isPublished ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_meta_rtc_YangRtcNative_sendH264Frame(JNIEnv *env, jclass clazz, jlong handle,
		jbyteArray data, jlong ptsUs, jboolean keyFrame) {
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	YangFrame frame;
	jsize len;
	jbyte *buf;

	if (rtc == NULL || rtc->vbuf == NULL || data == NULL)
		return 1;

	len = env->GetArrayLength(data);
	if (len <= 0)
		return 1;

	buf = env->GetByteArrayElements(data, NULL);
	if (buf == NULL)
		return 1;

	memset(&frame, 0, sizeof(frame));
	frame.mediaType = YangFrameTypeVideo;
	frame.frametype = keyFrame ? YANG_Frametype_I : YANG_Frametype_P;
	frame.payload = (uint8_t *) buf;
	frame.nb = (int32_t) len;
	frame.pts = (int64_t) ptsUs;

	rtc->vbuf->putEVideo(&frame);

	env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
	return Yang_Ok;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_meta_rtc_YangRtcNative_startPlay(JNIEnv *env, jclass clazz, jlong handle,
		jstring url, jobject surface, jboolean hwDecode) {
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	ANativeWindow *win;
	const char *curl;
	int32_t err;

	if (rtc == NULL || url == NULL || surface == NULL)
		return 1;

	win = ANativeWindow_fromSurface(env, surface);
	if (win == NULL)
		return 1;

	curl = env->GetStringUTFChars(url, NULL);
	if (curl == NULL) {
		ANativeWindow_release(win);
		return 1;
	}

	err = playStart(rtc, curl, win, hwDecode ? 1 : 0);
	env->ReleaseStringUTFChars(url, curl);
	return err;
}

extern "C" JNIEXPORT void JNICALL
Java_org_meta_rtc_YangRtcNative_stopPlay(JNIEnv *env, jclass clazz, jlong handle) {
	(void) env;
	(void) clazz;
	YangAndroidRtc *rtc = asRtc(handle);
	if (rtc != NULL)
		rtc->stopPlay();
}
