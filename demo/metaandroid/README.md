# metaandroid — metaRTC Android 全链路 WebRTC 直播 Demo

Kotlin Multiplatform 工程 + NDK JNI，复用仓库内的 metaRTC C/C++ 库：

```
CameraX 采集 → MediaCodec 硬编 H.264 → JNI → metaRTC(WHIP) → SRS
SRS → metaRTC(WHEP) → MediaCodec(NDK) 解码 → I420 → SurfaceView
```

与海思板端 `hisi_push` 的结构一致：**编码由硬件完成，metaRTC 只负责协议**。

---

## 目录结构

```
demo/metaandroid/
├── shared/                      KMP 公共模块
│   ├── commonMain/              YangRtc（expect，平台无关 API）
│   └── androidMain/             actual 实现 + JNI 声明（YangRtcNative）
├── androidApp/                  Compose UI
│   └── src/main/
│       ├── java/org/meta/rtc/demo/
│       │   ├── MainActivity.kt     界面与推拉流控制
│       │   └── CameraEncoder.kt    CameraX + MediaCodec 编码
│       └── cpp/
│           ├── yang_rtc_jni.cpp    JNI：WHIP 推流 / WHEP 拉流 / 解码
│           ├── yang_render.cpp     I420 → ANativeWindow
│           └── CMakeLists.txt
└── README.md
```

仓库根目录相关脚本：

| 脚本 | 作用 |
|---|---|
| `thirdparty/build_android_thirdparty.sh` | 交叉编译 openssl / libsrtp2 / usrsctp |
| `cmake_lib_android.sh` | 交叉编译 yangutil8 / metartccore8 / yangwhip8 / metartc8 |

---

## 前置条件

- Android SDK（含 NDK 25.1.8937393、CMake 3.22.1、build-tools 36）
- JDK 17+
- `local.properties` 里的 `sdk.dir` 指向本机 SDK（已填 `/home/dkjar/Android/Sdk`）

NDK 路径自动探测，也可显式指定：

```sh
export ANDROID_NDK=$HOME/Android/Sdk/ndk/25.1.8937393
```

默认 `ANDROID_API=29`（minSdk 29）。metaRTC 的 `YangEncoderMediacodec` 用到
API 28/29 才有的 NDK 接口，低于 29 会编译失败。

---

## 编译步骤

### 1. 第三方库

```sh
cd /home/dkjar/open/metaRTC
./thirdparty/build_android_thirdparty.sh
```

产物：`thirdparty/lib/android/arm64-v8a/lib/{libssl.a, libcrypto.a, libsrtp2.a, libusrsctp.a, libopus.a}`

### 2. metaRTC 静态库

```sh
./cmake_lib_android.sh
```

产物：`bin/lib_android/arm64-v8a/{libyangutil8.a, libmetartccore8.a, libyangwhip8.a, libmetartc8.a}`

### 3. APK

```sh
cd demo/metaandroid
./gradlew :androidApp:assembleDebug
```

产物：`androidApp/build/outputs/apk/debug/androidApp-debug.apk`

或直接用 Android Studio 打开 `demo/metaandroid` 运行。

---

## 使用

1. 安装 App，允许摄像头权限
2. 填写 **WHIP 推流地址**（默认 `http://192.168.100.188:1985/rtc/v1/whip/?app=live&stream=livestream`）
3. 点「开始推流」，摄像头预览出现即开始发送 H.264
4. 填写 **WHEP 拉流地址**（默认同一路流的 `/rtc/v1/whep/`）
5. 点「开始播放」，远端画面渲染在下方 SurfaceView

状态栏会显示已发送帧数与关键帧数；`logcat -s YangRtcJni` 可看 native 日志。

---

## 关键实现说明

### 时间戳单位

`CameraEncoder` 用 `ImageProxy.imageInfo.timestamp / 1000` 得到**微秒**，
JNI 透传给 metaRTC。metaRTC 内部按 `pts * 9 / 100` 换算成 90kHz RTP 时间戳，
**必须传微秒**，传毫秒会导致播放卡顿、延迟累积（与海思 `hisi_push` 踩过的坑相同）。

### 关键帧 SPS/PPS

MediaCodec 输出关键帧时不一定带 SPS/PPS，`CameraEncoder` 在 `INFO_OUTPUT_FORMAT_CHANGED`
时缓存 `csd-0`/`csd-1`，遇到关键帧且不含 SPS 时自动前置补齐。

### 拉流解码

Android 上 `YangDecoderFactory` 会自动选择 `YangDecoderMediacodec`（NDK AMediaCodec），
不需要额外引入 openh264/ffmpeg。解码结果为 I420，由 `yang_render.cpp` 直接写入
`ANativeWindow`（RGBA_8888），不经过 Kotlin 层，避免每帧跨 JNI 拷贝。

### 色彩格式适配

`CameraEncoder` 会查询编码器支持的色彩格式，优先 `COLOR_FormatYUV420SemiPlanar`(NV12)，
其次 `COLOR_FormatYUV420Planar`(I420)，最后回退 `COLOR_FormatYUV420Flexible`。
不同厂商芯片偏好不同，这里做了运行时适配。

---

## 已知限制

- 只推视频，未接音频（需要 opus 交叉编译，链路结构与音频一致）
- 仅 arm64-v8a；需要 armeabi-v7a 时给脚本传 `ABI=armeabi-v7a` 并在 gradle 里加 abiFilter
- 部分设备编码器不接受 ByteBuffer 形式的 YUV 输入，此时需要改成 `createInputSurface` + EGL 送帧
- 拉流分辨率跟随对端，首次收到 SPS 后才建立解码器（约 1 帧延迟）
