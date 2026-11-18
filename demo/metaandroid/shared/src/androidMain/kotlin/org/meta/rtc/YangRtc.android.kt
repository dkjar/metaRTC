package org.meta.rtc

import android.view.Surface

actual class YangRtc actual constructor() {

    private var handle: Long = YangRtcNative.create()

    actual fun startPublish(url: String): Int {
        if (handle == 0L) return 1
        return YangRtcNative.startPublish(handle, url)
    }

    actual fun stopPublish() {
        if (handle == 0L) return
        YangRtcNative.stopPublish(handle)
    }

    actual fun isPublishing(): Boolean {
        if (handle == 0L) return false
        return YangRtcNative.isPublishing(handle)
    }

    actual fun sendH264Frame(data: ByteArray, ptsUs: Long, keyFrame: Boolean): Int {
        if (handle == 0L) return 1
        return YangRtcNative.sendH264Frame(handle, data, ptsUs, keyFrame)
    }

    /**
     * 以 WHEP 拉流，并把解码结果直接渲染到 [surface]
     *
     * @param hwDecode true=MediaCodec 硬解（失败自动回退 FFmpeg 软解）, false=FFmpeg 软解
     * 注意：[surface] 必须是已经创建好的 Surface（SurfaceView 的 surfaceCreated 之后）
     */
    fun startPlay(url: String, surface: Surface, hwDecode: Boolean = true): Int {
        if (handle == 0L) return 1
        return YangRtcNative.startPlay(handle, url, surface, hwDecode)
    }

    fun stopPlay() {
        if (handle == 0L) return
        YangRtcNative.stopPlay(handle)
    }

    actual fun release() {
        if (handle == 0L) return
        YangRtcNative.destroy(handle)
        handle = 0L
    }
}

internal object YangRtcNative {

    init {
        System.loadLibrary("yangrtc")
    }

    @JvmStatic
    external fun create(): Long

    @JvmStatic
    external fun destroy(handle: Long)

    @JvmStatic
    external fun startPublish(handle: Long, url: String): Int

    @JvmStatic
    external fun stopPublish(handle: Long)

    @JvmStatic
    external fun isPublishing(handle: Long): Boolean

    @JvmStatic
    external fun sendH264Frame(handle: Long, data: ByteArray, ptsUs: Long, keyFrame: Boolean): Int

    @JvmStatic
    external fun startPlay(handle: Long, url: String, surface: Surface, hwDecode: Boolean): Int

    @JvmStatic
    external fun stopPlay(handle: Long)
}
