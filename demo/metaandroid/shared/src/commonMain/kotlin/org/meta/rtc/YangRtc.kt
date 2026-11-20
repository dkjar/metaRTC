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

    /**
     * 轮询拉流统计（用于播放控件显示运行状态）。
     *
     * 返回 `long[8]` = { 累计收帧 rx, 累计解码 dec, 累计丢帧 drop,
     *                   宽度, 高度, 解码器类型(1=硬解/0=软解/-1=未初始化),
     *                   是否已连接(0/1), 是否播放中(0/1) }。
     * 两次调用的差值即为对应速率。
     */
    fun getPlayStats(): LongArray

    /** 释放全部 native 资源 */
    fun release()
}
