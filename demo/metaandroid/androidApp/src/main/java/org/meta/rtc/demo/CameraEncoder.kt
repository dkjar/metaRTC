package org.meta.rtc.demo

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import java.nio.ByteBuffer

/**
 * CameraX 采集 + MediaCodec 硬件 H.264 编码
 *
 * 产出的每一帧都是 Annex-B 格式（关键帧自动补 SPS/PPS），
 * 与海思板端 VENC 的输出形式一致，直接交给 YangRtc.sendH264Frame。
 */
class CameraEncoder(
    private val context: Context,
    private val onFrame: (data: ByteArray, ptsUs: Long, keyFrame: Boolean) -> Unit
) {
    companion object {
        private const val TAG = "CameraEncoder"
    }

    private val thread = HandlerThread("camera-encoder").apply { start() }
    private val handler = Handler(thread.looper)

    private var codec: MediaCodec? = null
    private var colorFormat: Int = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible
    private var csd: ByteArray? = null
    private var cameraProvider: ProcessCameraProvider? = null
    private var analysis: ImageAnalysis? = null

    var frameCount: Long = 0
        private set

    var keyFrameCount: Long = 0
        private set

    fun start(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        width: Int = 1280,
        height: Int = 720,
        fps: Int = 30,
        bitrate: Int = 2_000_000
    ) {
        val future = ProcessCameraProvider.getInstance(context)
        future.addListener({
            try {
                val provider = future.get()
                cameraProvider = provider

                val preview = Preview.Builder().build().also {
                    it.setSurfaceProvider(previewView.surfaceProvider)
                }

                val imageAnalysis = ImageAnalysis.Builder()
                    .setTargetResolution(Size(width, height))
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
                    .build()
                analysis = imageAnalysis

                initEncoder(width, height, fps, bitrate)

                imageAnalysis.setAnalyzer({ runnable -> handler.post(runnable) }) { proxy -> encode(proxy) }

                provider.unbindAll()
                provider.bindToLifecycle(
                    lifecycleOwner,
                    CameraSelector.DEFAULT_BACK_CAMERA,
                    preview,
                    imageAnalysis
                )
                Log.i(TAG, "camera started ${width}x$height @$fps $bitrate")
            } catch (e: Exception) {
                Log.e(TAG, "start camera failure", e)
            }
        }, ContextCompat.getMainExecutor(context))
    }

    private fun initEncoder(width: Int, height: Int, fps: Int, bitrate: Int) {
        releaseEncoder()

        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        colorFormat = chooseColorFormat(codec)

        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, colorFormat)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            // 1 秒一个 I 帧, 兼顾起播速度与码率
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(
                MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            )
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
        }

        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        codec.start()
        this.codec = codec
        csd = null
        Log.i(TAG, "encoder started, colorFormat=$colorFormat")
    }

    private fun chooseColorFormat(codec: MediaCodec): Int {
        return try {
            val caps = codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
            val formats = caps.colorFormats.toSet()
            when {
                formats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar) ->
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar
                formats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar) ->
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
                else -> MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible
            }
        } catch (e: Exception) {
            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible
        }
    }

    private fun encode(proxy: ImageProxy) {
        val codec = this.codec
        if (codec == null) {
            proxy.close()
            return
        }

        try {
            val w = proxy.width
            val h = proxy.height
            val data = when (colorFormat) {
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar -> yuv420ToNv12(proxy, w, h)
                else -> yuv420ToI420(proxy, w, h)
            }

            val index = codec.dequeueInputBuffer(10_000)
            if (index >= 0) {
                val buf = codec.getInputBuffer(index)
                if (buf != null) {
                    buf.clear()
                    buf.put(data)
                    // CameraX 的 timestamp 是纳秒, metaRTC 需要微秒
                    val ptsUs = proxy.imageInfo.timestamp / 1000
                    codec.queueInputBuffer(index, 0, data.size, ptsUs, 0)
                }
            }

            drain(codec)
        } catch (e: Exception) {
            Log.e(TAG, "encode failure", e)
        } finally {
            proxy.close()
        }
    }

    private fun drain(codec: MediaCodec) {
        val info = MediaCodec.BufferInfo()

        while (true) {
            val index = codec.dequeueOutputBuffer(info, 0)
            when {
                index == MediaCodec.INFO_TRY_AGAIN_LATER -> return

                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val fmt = codec.outputFormat
                    val s0 = fmt.getByteBuffer("csd-0")
                    val s1 = fmt.getByteBuffer("csd-1")
                    csd = concat(csdOf(s0), csdOf(s1))
                    Log.i(TAG, "csd updated, size=${csd?.size ?: 0}")
                }

                index >= 0 -> {
                    val out = codec.getOutputBuffer(index)
                    if (out != null && info.size > 0) {
                        out.position(info.offset)
                        out.limit(info.offset + info.size)
                        val raw = ByteArray(info.size)
                        out.get(raw)

                        val key = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                        var payload: ByteArray = raw
                        // 关键帧若不带 SPS/PPS, 用 csd 补齐, 否则对端解不出来
                        if (key && csd != null && !containsSps(raw)) {
                            payload = concat(csd, raw)
                        }

                        frameCount++
                        if (key) keyFrameCount++
                        onFrame(payload, info.presentationTimeUs, key)
                    }
                    codec.releaseOutputBuffer(index, false)
                }
            }
        }
    }

    fun stop() {
        cameraProvider?.unbindAll()
        cameraProvider = null
        analysis?.clearAnalyzer()
        analysis = null

        handler.post { releaseEncoder() }
    }

    private fun releaseEncoder() {
        try {
            codec?.stop()
        } catch (e: Exception) {
            // ignore
        }
        try {
            codec?.release()
        } catch (e: Exception) {
            // ignore
        }
        codec = null
    }

    // ------------------------------------------------------------ YUV 转换

    private fun yuv420ToI420(proxy: ImageProxy, w: Int, h: Int): ByteArray {
        val out = ByteArray(w * h * 3 / 2)
        copyPlane(proxy.planes[0], w, h, out, 0)
        copyPlane(proxy.planes[1], w / 2, h / 2, out, w * h)
        copyPlane(proxy.planes[2], w / 2, h / 2, out, w * h + w * h / 4)
        return out
    }

    private fun yuv420ToNv12(proxy: ImageProxy, w: Int, h: Int): ByteArray {
        val out = ByteArray(w * h * 3 / 2)
        copyPlane(proxy.planes[0], w, h, out, 0)

        val u = planeToArray(proxy.planes[1], w / 2, h / 2)
        val v = planeToArray(proxy.planes[2], w / 2, h / 2)
        var pos = w * h
        for (i in 0 until w * h / 4) {
            out[pos++] = u[i]
            out[pos++] = v[i]
        }
        return out
    }

    private fun copyPlane(
        plane: ImageProxy.PlaneProxy,
        width: Int,
        height: Int,
        dst: ByteArray,
        offset: Int
    ) {
        val src = plane.buffer.duplicate()
        val rowStride = plane.rowStride
        val pixelStride = plane.pixelStride
        var pos = offset
        val row = ByteArray(rowStride)

        for (r in 0 until height) {
            val start = r * rowStride
            if (start >= src.limit()) break
            src.position(start)
            val len = minOf(rowStride, src.remaining())
            src.get(row, 0, len)
            var i = 0
            for (c in 0 until width) {
                if (i >= len) break
                dst[pos++] = row[i]
                i += pixelStride
            }
        }
    }

    private fun planeToArray(plane: ImageProxy.PlaneProxy, width: Int, height: Int): ByteArray {
        val out = ByteArray(width * height)
        copyPlane(plane, width, height, out, 0)
        return out
    }

    private fun csdOf(buffer: ByteBuffer?): ByteArray? {
        if (buffer == null) return null
        val b = buffer.duplicate()
        val out = ByteArray(b.remaining())
        b.get(out)
        return out
    }

    private fun concat(a: ByteArray?, b: ByteArray?): ByteArray {
        if (a == null || a.isEmpty()) return b ?: ByteArray(0)
        if (b == null || b.isEmpty()) return a
        val out = ByteArray(a.size + b.size)
        System.arraycopy(a, 0, out, 0, a.size)
        System.arraycopy(b, 0, out, a.size, b.size)
        return out
    }

    private fun containsSps(data: ByteArray): Boolean {
        var i = 0
        while (i + 4 < data.size) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 && data[i + 2].toInt() == 1) {
                return (data[i + 3].toInt() and 0x1f) == 7
            }
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0 &&
                data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1
            ) {
                return (data[i + 4].toInt() and 0x1f) == 7
            }
            i++
        }
        return false
    }
}
