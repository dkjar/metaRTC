package org.meta.rtc

/**
 * 全链路 WebRTC 客户端（推流 + 拉流）。
 *
 * 公共 API 只包含与平台无关的部分：
 *  - 推流：调用方把编码后的 H.264 Annex-B 帧交给 [sendH264Frame]
 *  - 拉流：平台相关的渲染入口放在 androidMain（见 `YangRtc.startPlay(url, surface)`）
 */
expect class YangRtc() {

    /** 连接 WHIP 推流地址，返回 0 表示成功 */
    fun startPublish(url: String): Int

    /** 断开推流 */
    fun stopPublish()

    /** 是否已完成推流协商（可以开始送帧） */
    fun isPublishing(): Boolean

    /**
     * 送入一帧 H.264 Annex-B 数据
     *
     * @param ptsUs 采集时间戳，单位微秒（metaRTC 内部按微秒换算 90kHz RTP 时间戳）
     * @param keyFrame 是否关键帧（IDR）
     */
    fun sendH264Frame(data: ByteArray, ptsUs: Long, keyFrame: Boolean): Int

    /** 释放全部 native 资源 */
    fun release()
}
