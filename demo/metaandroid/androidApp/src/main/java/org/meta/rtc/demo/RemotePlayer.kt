package org.meta.rtc.demo

import android.app.Activity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Fullscreen
import androidx.compose.material.icons.filled.FullscreenExit
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.meta.rtc.YangRtc

/**
 * 仿 KitePlayer 风格的播放控件。
 *
 * 播放内核仍是 metaRTC WHEP（[rtc]），这里只负责在 [SurfaceView] 之上叠加
 * KitePlayer 那样的控制层：点击显隐 / 自动隐藏、中央播放键、底部 LIVE+全屏
 * 控制条、状态叠层，以及 rx/dec/drop 运行统计。
 *
 * 注意：[fullscreen] 切换只改变外层 Modifier，不重建 [SurfaceView]，
 * 否则 native 侧持有的 ANativeWindow 会失效。
 */
@Composable
fun RemotePlayer(
    rtc: YangRtc,
    whepUrl: String,
    fullscreen: Boolean,
    onToggleFullscreen: () -> Unit,
    onStatus: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()

    var phase by remember { mutableStateOf(PlayerPhase.Idle) }
    var errorMsg by remember { mutableStateOf<String?>(null) }
    var surface by remember { mutableStateOf<Surface?>(null) }
    var controlsVisible by remember { mutableStateOf(true) }
    var elapsedSec by remember { mutableLongStateOf(0L) }
    val stats = remember { PlayerStatsState() }

    val streamName = remember(whepUrl) {
        whepUrl.substringAfter("stream=", "").substringBefore("&").ifEmpty { "直播" }
    }

    // 控件自动隐藏：播放中且可见时，3 秒无操作收起
    LaunchedEffect(controlsVisible, phase) {
        if (phase == PlayerPhase.Playing && controlsVisible) {
            delay(3000)
            controlsVisible = false
        }
    }

    // 已播时长（直播流从拉流成功开始计）
    LaunchedEffect(phase) {
        if (phase != PlayerPhase.Playing) {
            elapsedSec = 0L
            return@LaunchedEffect
        }
        while (true) {
            delay(1000)
            elapsedSec++
        }
    }

    // 每秒轮询 native 统计，计算速率
    LaunchedEffect(phase) {
        if (phase != PlayerPhase.Playing) return@LaunchedEffect
        var prevRx = 0L
        var prevDec = 0L
        var prevDrop = 0L
        while (true) {
            val raw = withContext(Dispatchers.IO) { rtc.getPlayStats() }
            if (raw.size >= 8) {
                stats.rxRate = (raw[0] - prevRx).toInt().coerceAtLeast(0).also { prevRx = raw[0] }
                stats.decRate = (raw[1] - prevDec).toInt().coerceAtLeast(0).also { prevDec = raw[1] }
                stats.dropRate = (raw[2] - prevDrop).toInt().coerceAtLeast(0).also { prevDrop = raw[2] }
                stats.width = raw[3].toInt()
                stats.height = raw[4].toInt()
                stats.decoderType = raw[5].toInt()
                stats.connected = raw[6] == 1L
            }
            delay(1000)
        }
    }

    // 全屏时隐藏系统栏，退出时恢复
    DisposableEffect(fullscreen) {
        val window = (context as? Activity)?.window
        if (window != null) {
            val controller = WindowInsetsControllerCompat(window, window.decorView)
            if (fullscreen) {
                WindowCompat.setDecorFitsSystemWindows(window, false)
                controller.hide(WindowInsetsCompat.Type.systemBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
            onDispose {
                WindowCompat.setDecorFitsSystemWindows(window, true)
                controller.show(WindowInsetsCompat.Type.systemBars())
            }
        } else {
            onDispose {}
        }
    }

    fun startPlayback() {
        val s = surface ?: return
        phase = PlayerPhase.Connecting
        errorMsg = null
        controlsVisible = true
        onStatus("正在拉流 ...")
        scope.launch {
            val ret = withContext(Dispatchers.IO) { rtc.startPlay(whepUrl, s) }
            if (ret != 0) {
                phase = PlayerPhase.Error
                errorMsg = "拉流失败($ret)"
                onStatus("拉流失败($ret)")
            } else {
                phase = PlayerPhase.Playing
                onStatus("拉流中")
            }
        }
    }

    fun stopPlayback() {
        scope.launch(Dispatchers.IO) { rtc.stopPlay() }
        phase = PlayerPhase.Idle
        controlsVisible = true
        elapsedSec = 0L
        stats.reset()
        onStatus("播放已停止")
    }

    val shape = if (fullscreen) RoundedCornerShape(0.dp) else RoundedCornerShape(12.dp)

    Box(
        modifier = modifier
            .clip(shape)
            .background(Color.Black)
    ) {
        // 视频层：metaRTC 把解码结果直出到这个 Surface
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            surface = holder.surface
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            surface = null
                        }
                    })
                }
            }
        )

        // 点击空白区域切换控件显隐
        Box(
            Modifier
                .fillMaxSize()
                .clickable { controlsVisible = !controlsVisible }
        )

        val showControls = controlsVisible || phase != PlayerPhase.Playing

        // 顶部渐变 + 标题栏
        if (showControls) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(56.dp)
                    .background(
                        Brush.verticalGradient(
                            listOf(Color.Black.copy(alpha = 0.6f), Color.Transparent)
                        )
                    )
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(horizontal = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    LiveDot(visible = phase == PlayerPhase.Playing)
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = streamName,
                        color = Color.White,
                        style = MaterialTheme.typography.titleSmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                    if (fullscreen) {
                        IconButton(onClick = onToggleFullscreen) {
                            Icon(Icons.Filled.Close, contentDescription = "退出全屏", tint = Color.White)
                        }
                    }
                }
            }
        }

        // 统计叠层（播放中常驻，便于排查卡顿）
        if (phase == PlayerPhase.Playing) {
            Text(
                text = buildString {
                    append("rx ${stats.rxRate}  dec ${stats.decRate}  drop ${stats.dropRate}")
                    if (stats.width > 0) append("  ·  ${stats.width}×${stats.height}")
                    append("  ·  ").append(stats.decoderLabel())
                    if (!stats.connected) append("  ·  未连接")
                },
                color = Color.White.copy(alpha = 0.85f),
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(top = 60.dp, start = 12.dp)
                    .background(Color.Black.copy(alpha = 0.35f), RoundedCornerShape(4.dp))
                    .padding(horizontal = 6.dp, vertical = 2.dp)
            )
        }

        // 中央播放/暂停键：空闲时常驻，播放中仅在控件可见时显示
        val showCenter = phase == PlayerPhase.Idle ||
                (phase == PlayerPhase.Playing && controlsVisible)
        if (showCenter) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                IconButton(
                    onClick = {
                        if (phase == PlayerPhase.Playing) stopPlayback() else startPlayback()
                    },
                    modifier = Modifier
                        .size(64.dp)
                        .background(Color.Black.copy(alpha = 0.5f), CircleShape)
                ) {
                    Icon(
                        imageVector = if (phase == PlayerPhase.Playing) Icons.Filled.Pause
                        else Icons.Filled.PlayArrow,
                        contentDescription = if (phase == PlayerPhase.Playing) "停止" else "播放",
                        tint = Color.White,
                        modifier = Modifier.size(36.dp)
                    )
                }
            }
        }

        // 连接中
        if (phase == PlayerPhase.Connecting) {
            Column(
                Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                CircularProgressIndicator(color = Color.White)
                Spacer(Modifier.height(10.dp))
                Text("连接中…", color = Color.White)
            }
        }

        // 错误叠层 + 重试
        if (phase == PlayerPhase.Error) {
            Column(
                Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text(errorMsg ?: "播放出错", color = Color.White)
                Spacer(Modifier.height(10.dp))
                IconButton(
                    onClick = { startPlayback() },
                    modifier = Modifier.background(Color.White.copy(alpha = 0.15f), CircleShape)
                ) {
                    Icon(Icons.Filled.Refresh, contentDescription = "重试", tint = Color.White)
                }
            }
        }

        // 底部渐变 + 控制条
        if (showControls) {
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(64.dp)
                    .background(
                        Brush.verticalGradient(
                            listOf(Color.Transparent, Color.Black.copy(alpha = 0.6f))
                        )
                    )
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(horizontal = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = formatElapsed(elapsedSec),
                        color = Color.White,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    Spacer(Modifier.width(8.dp))
                    if (stats.width > 0) {
                        Text(
                            text = "${stats.height}p",
                            color = Color.White.copy(alpha = 0.9f),
                            fontSize = 12.sp
                        )
                        Spacer(Modifier.width(8.dp))
                    }
                    Text(
                        text = stats.decoderShortLabel(),
                        color = Color.White.copy(alpha = 0.9f),
                        fontSize = 12.sp
                    )
                    Spacer(Modifier.weight(1f))
                    IconButton(onClick = onToggleFullscreen) {
                        Icon(
                            imageVector = if (fullscreen) Icons.Filled.FullscreenExit
                            else Icons.Filled.Fullscreen,
                            contentDescription = if (fullscreen) "退出全屏" else "全屏",
                            tint = Color.White
                        )
                    }
                }
            }
        }
    }
}

private enum class PlayerPhase { Idle, Connecting, Playing, Error }

@Stable
private class PlayerStatsState {
    var rxRate by mutableIntStateOf(0)
    var decRate by mutableIntStateOf(0)
    var dropRate by mutableIntStateOf(0)
    var width by mutableIntStateOf(0)
    var height by mutableIntStateOf(0)
    var decoderType by mutableIntStateOf(-1)
    var connected by mutableStateOf(false)

    fun reset() {
        rxRate = 0; decRate = 0; dropRate = 0
        width = 0; height = 0; decoderType = -1; connected = false
    }

    fun decoderLabel(): String = when (decoderType) {
        1 -> "MediaCodec 硬解"
        0 -> "FFmpeg 软解"
        else -> "解码器未初始化"
    }

    fun decoderShortLabel(): String = when (decoderType) {
        1 -> "硬解"
        0 -> "软解"
        else -> "-"
    }
}

@Composable
private fun LiveDot(visible: Boolean) {
    val transition = rememberInfiniteTransition(label = "live-dot")
    val scale by transition.animateFloat(
        initialValue = 1f,
        targetValue = 1.45f,
        animationSpec = infiniteRepeatable(
            animation = tween(700),
            repeatMode = RepeatMode.Reverse
        ),
        label = "live-dot-scale"
    )
    Box(
        Modifier
            .size(10.dp)
            .graphicsLayer {
                scaleX = if (visible) scale else 1f
                scaleY = if (visible) scale else 1f
            }
            .background(
                if (visible) Color.Red else Color.White.copy(alpha = 0.5f),
                CircleShape
            )
    )
}

private fun formatElapsed(sec: Long): String {
    val m = sec / 60
    val s = sec % 60
    return "%02d:%02d".format(m, s)
}
