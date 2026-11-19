package org.meta.rtc.demo

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.meta.rtc.YangRtc

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                MainScreen()
            }
        }
    }
}

@Composable
private fun MainScreen() {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()

    var whipUrl by remember {
        mutableStateOf("http://47.76.137.109:1985/rtc/v1/whip/?app=live&stream=livestream")
    }
    var whepUrl by remember {
        mutableStateOf("http://47.76.137.109:1985/rtc/v1/whep/?app=live&stream=livestream")
    }

    val rtc = remember { YangRtc() }
    val encoder = remember {
        CameraEncoder(context) { data, ptsUs, keyFrame ->
            rtc.sendH264Frame(data, ptsUs, keyFrame)
        }
    }

    var publishing by remember { mutableStateOf(false) }
    var playing by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("就绪") }
    var hasCamera by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                    PackageManager.PERMISSION_GRANTED
        )
    }

    var previewView by remember { mutableStateOf<PreviewView?>(null) }
    var surface by remember { mutableStateOf<Surface?>(null) }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        hasCamera = granted
        if (!granted) status = "未授权摄像头，无法推流"
    }

    LaunchedEffect(Unit) {
        if (!hasCamera) permissionLauncher.launch(Manifest.permission.CAMERA)
    }

    // 每秒刷新一次状态
    LaunchedEffect(publishing, playing) {
        while (publishing || playing) {
            if (publishing) {
                status = "推流中：${encoder.frameCount} 帧（关键帧 ${encoder.keyFrameCount}）"
            }
            delay(1000)
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            encoder.stop()
            rtc.stopPlay()
            rtc.release()
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(12.dp)
            .verticalScroll(rememberScrollState())
    ) {
        Text("metaRTC 全链路 WebRTC 直播", style = MaterialTheme.typography.titleMedium)

        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = whipUrl,
            onValueChange = { whipUrl = it },
            label = { Text("WHIP 推流地址") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(
            value = whepUrl,
            onValueChange = { whepUrl = it },
            label = { Text("WHEP 拉流地址") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )

        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                enabled = !publishing && hasCamera,
                onClick = {
                    val pv = previewView
                    if (pv == null) {
                        status = "预览未就绪"
                        return@Button
                    }
                    scope.launch {
                        status = "正在连接 SRS ..."
                        val ret = withContext(Dispatchers.IO) { rtc.startPublish(whipUrl) }
                        if (ret != 0) {
                            status = "推流建链失败($ret)"
                            return@launch
                        }
                        encoder.start(lifecycleOwner, pv)
                        publishing = true
                        status = "推流中"
                    }
                }
            ) { Text("开始推流") }

            Button(
                enabled = publishing,
                onClick = {
                    encoder.stop()
                    rtc.stopPublish()
                    publishing = false
                    status = "推流已停止"
                }
            ) { Text("停止推流") }
        }

        Spacer(Modifier.height(8.dp))
        Text("摄像头预览", style = MaterialTheme.typography.labelLarge)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(170.dp)
                .background(Color.Black)
        ) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { ctx ->
                    PreviewView(ctx).also { previewView = it }
                }
            )
        }

        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                enabled = !playing && surface != null,
                onClick = {
                    val s = surface ?: return@Button
                    scope.launch {
                        status = "正在拉流 ..."
                        val ret = withContext(Dispatchers.IO) { rtc.startPlay(whepUrl, s) }
                        if (ret != 0) {
                            status = "拉流失败($ret)"
                            return@launch
                        }
                        playing = true
                        status = "拉流中"
                    }
                }
            ) { Text("开始播放") }

            Button(
                enabled = playing,
                onClick = {
                    rtc.stopPlay()
                    playing = false
                    status = "播放已停止"
                }
            ) { Text("停止播放") }

            Spacer(Modifier.width(4.dp))
        }

        Spacer(Modifier.height(8.dp))
        Text("远端画面", style = MaterialTheme.typography.labelLarge)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(230.dp)
                .background(Color.Black)
        ) {
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
        }

        Spacer(Modifier.height(10.dp))
        Text(status, style = MaterialTheme.typography.bodySmall)
        Spacer(Modifier.height(16.dp))
    }
}
