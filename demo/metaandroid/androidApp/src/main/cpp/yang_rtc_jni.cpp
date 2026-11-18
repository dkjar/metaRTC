//
// metaRTC Android JNI 桥接
//   推流: Kotlin(CameraX + MediaCodec) 产出 H.264 Annex-B -> sendH264Frame -> WHIP
//   拉流: WHEP 收流 -> MediaCodec(NDK) 解码 -> I420 直接画到 SurfaceView
//
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pthread.h>
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
	pthread_t playThread;
	volatile int32_t playing;
	volatile int32_t playStarted;
	char *playUrl;

	YangAndroidRtc() {
		pubCtx = NULL;
		vbuf = NULL;
		pub = NULL;
		playCtx = NULL;
		recv = NULL;
		dec = NULL;
		render = new YangAndroidRender();
		playing = 0;
		playStarted = 0;
		playUrl = NULL;
	}

	~YangAndroidRtc() {
		stopPublish();
		stopPlay();
		if (render) {
			delete render;
			render = NULL;
		}
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
	if (rtc->dec == NULL)
		return;

	// 收到 SPS 时解析分辨率, 同时初始化解码器
	if (rtc->cb.width <= 0 && hasSps(frame->payload, frame->nb)) {
		int32_t w = 0, h = 0, fps = 0;
		rtc->dec->parseHeader(frame->payload, frame->nb, &w, &h, &fps);

		// 硬解初始化失败 (MediaCodec configure/start 失败) -> 回退 FFmpeg 软解
		if (rtc->dec->m_isInit == 0 && rtc->playCtx->avinfo.video.videoDecHwType != 0) {
			ALOGE("hardware decoder init failed, fallback to FFmpeg software decode");
			delete rtc->dec;
			rtc->dec = NULL;
			rtc->playCtx->avinfo.video.videoDecHwType = 0;
			YangDecoderFactory factory;
			rtc->dec = factory.createVideoDecoder(Yang_VED_H264, &rtc->playCtx->avinfo.video);
			if (rtc->dec != NULL) {
				rtc->dec->init();
				rtc->dec->parseHeader(frame->payload, frame->nb, &w, &h, &fps);
			}
		}

		if (w > 0 && h > 0) {
			rtc->cb.width = w;
			rtc->cb.height = h;
			ALOGI("remote video %dx%d @%d", w, h, fps);
		}
	}

	if (rtc->dec != NULL)
		rtc->dec->decode(frame, YangI420, &rtc->cb);
}

static void *playThreadRun(void *arg) {
	YangAndroidRtc *rtc = (YangAndroidRtc *) arg;
	int32_t err;

	YangDecoderFactory factory;
	rtc->dec = factory.createVideoDecoder(Yang_VED_H264, &rtc->playCtx->avinfo.video);
	if (rtc->dec != NULL)
		rtc->dec->init();

	err = yang_whip_connectWhipWhepServer(&rtc->recv->peer, rtc->playUrl);
	if (err)
		ALOGE("whep connect failure, err=%d", err);
	else
		ALOGI("whep connected: %s", rtc->playUrl);

	while (rtc->playing)
		usleep(100 * 1000);

	if (rtc->recv != NULL)
		rtc->recv->close(&rtc->recv->peer);

	if (rtc->dec != NULL) {
		delete rtc->dec;
		rtc->dec = NULL;
	}
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
	rtc->cb.render = rtc->render;
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
		free(recv);
		recv = NULL;
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
