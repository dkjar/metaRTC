# metaandroid — metaRTC Android 全链路 WebRTC 直播 Demo

Kotlin Multiplatform 工程 + NDK JNI，复用仓库内的 metaRTC C/C++ 库：

```
CameraX 采集 → MediaCodec 硬编 H.264 → JNI → metaRTC(WHIP) → SRS
SRS → metaRTC(WHEP) → 独立解码线程 → MediaCodec(NDK) → Surface（硬解直出）
                                    └→ FFmpeg 软解 → I420 → ANativeWindow（回退路径）
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

- Android SDK（含 NDK 25.1.8937393、CMake 3.22.1、build-tools 36、platform android-36）
- JDK 17+（实测 JDK 21 可用）
- Gradle 8.13 / AGP 8.11.1 / Kotlin 2.2.20（wrapper 与根 `build.gradle.kts` 已锁定）
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

产物：`thirdparty/lib/android/arm64-v8a/lib/{libssl.a, libcrypto.a, libsrtp2.a, libusrsctp.a, libopus.a, libavcodec.a, libavutil.a}`
（`libavcodec.a` / `libavutil.a` 仅用于拉流软解回退）

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

Android 上 `YangDecoderFactory` 默认返回 `YangDecoderMediacodec`（NDK AMediaCodec 硬解），
**解码结果直连 Surface**：`AMediaCodec_configure(m_mediaCodec, fmt, surface, NULL, 0)` +
`releaseOutputBuffer(idx, true)`，不做任何 CPU 颜色转换。

拉流管线（见 `yang_rtc_jni.cpp`）：

```
UDP 收包线程: onRecvVideo → SPS 解析 → 深拷贝入队（4 帧，满则丢最旧）
解码线程    : queuePop → dec->decode() → MediaCodec → Surface
```

解码在独立线程执行，收包线程只做入队，避免解码慢时阻塞 RTP 收包引发丢包。
`videoDecHwType=0` 或硬解初始化失败时回退到 FFmpeg 软解（I420 经 `yang_render.cpp`
写 `ANativeWindow`）。

调试用统计日志（每 5 秒）：

```bash
adb logcat -s YangRtcJni | grep -E "decoder=|remote video|play stats|fallback"
# play stats: rx=150 dec=150 drop=0 in 5000ms
#   rx   收到的帧数    dec  已解码帧数    drop 队列丢弃帧数
```

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
- 软解回退路径仍是 CPU 转换（I420 → RGBA），1080p 可能跟不上，仅作兜底

---

## 修改记录

### 2026-11-19 拉流卡顿排查

- 修复 `AMediaCodec_start` 返回值未检查：此前 start 失败也会把 `m_isInit` 置 1，
  导致「硬解失败自动回退软解」永远不触发
- 显式设置 `AMEDIAFORMAT_KEY_MAX_INPUT_SIZE`（≥ 512KB，1080p 取 `w*h`）：
  避免大 I 帧超过输入 buffer 被整帧丢弃 —— 丢 IDR 会断开参考链，表现为持续卡顿
- 输入 buffer 放不下时调用 `queueInputBuffer(size=0)` 归还，修复输入 buffer 泄漏
- 新增 5 秒周期统计日志 `play stats: rx/dec/drop` 与解码器类型日志 `decoder=...`
- **修复快速运动花屏**：解码队列溢出时不再丢单个 P 帧（丢中间帧会导致后续帧
  失去参考而花屏），改为清空队列并丢弃到下一个关键帧，同时发送 PLI 请求 IDR
  （1 秒限频）；解码器连续取不到输出时也会请求 PLI

### 2026-11-16 拉流管线优化

- 硬解输出直连 Surface，去掉 NV12→I420、逐像素 I420→RGBA 两遍 CPU 全帧转换
- 解码从 UDP 收包线程移入独立线程 + 4 帧有界队列（满则丢最旧），避免拖慢收包
- 修正 PTS 单位：RTP 90kHz 时间戳转微秒后再交给 MediaCodec
- 解码循环一次排空所有已解码帧；已取到帧后的 `TRY_AGAIN` 不再误判为解码失败

### 更早

- 新增 FFmpeg 5.1.4（arm64-v8a，仅 h264/hevc 解码）交叉编译，作为软解回退；
  `YangDecoderFactory` 按 `videoDecHwType` 选择 MediaCodec / FFmpeg
- 工具链升级：Gradle 8.13、AGP 8.11.1、Kotlin 2.2.20、compileSdk 36；
  `kotlinOptions` 迁移为 `compilerOptions { jvmTarget }`
