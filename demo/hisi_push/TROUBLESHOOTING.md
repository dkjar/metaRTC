# hisi_push 推流问题排查与修改总结

平台：Hi3516CV610 + SC4336P + arm-v01c02-linux-musleabi 交叉编译
链路：VI/ISP/VPSS → VENC(H.264 硬编码) → metaRTC(WHIP) → SRS → WebRTC/FLV 播放

---

## 一、最终根因（最重要）

### RTP 时间戳单位错误 —— 卡顿、跳帧、延迟累积的真正原因

| 项 | 内容 |
|---|---|
| 根因 | 发送端把 MPP 的微秒 PTS 除了 1000 转成毫秒，而 metaRTC 的 `YangTimestamp.c` 用 `pts * 9 / 100` 做 **微秒 → 90kHz** 换算 |
| 结果 | RTP 时间戳比真实小 1000 倍：30fps 每帧 33ms，时间戳只前进 2 个单位（0.02ms） |
| 症状 | 播放器 jitter buffer 无法排播 → 转头卡顿、动作不连贯、延迟越积越大 |
| 修复 | `YangHisiVenc.cpp` 保留微秒；`main.cpp` 文件回放同步改微秒 |

调用链：

```
YangHisiVenc:  pts = MPP微秒 / 1000        ← 错误：转成毫秒
      ↓
YangPacer:     ts.setVideoTime(frame->pts)
      ↓
YangTimestamp: return pts * 9 / 100        ← 该公式要求输入为微秒
```

官方采集端 `YangVideoCaptureHandle.cpp` 使用 `yang_get_system_time`，
其定义为 `yang_get_system_micro_time`（微秒），证明 metaRTC 内部约定就是**微秒**。

### 证据（从 SRS 拉 FLV 解析）

```
关键帧 ts 序列: 11, 12, 13, 14, ... 23     ← 每个 GOP 只 +1ms
381 帧里只有 14 个不同的时间戳             ← 其余全重复
```

理论核算：一个 GOP 30 帧 = 1000ms，`1000 × 9 ÷ 100 = 90`，`90 ÷ 90 = 1ms`，与实测完全吻合。

---

## 二、推流链路的三个关键修复

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | DTLS 握手失败 `ARQ r0=-1` | 接收线程 `processData()` 与 ARQ 重传线程并发访问 SSL/`bioOut`，`BIO_reset` 冲掉握手 flight | `YangDtlsOpenssl.c`、`YangRtcDtls.h` 对 SSL/BIO 访问加互斥锁，内部重入前释放 |
| 2 | VENC `query_status 0xa0088009` | 依赖外部 sample 建通道，实际 sample 只建 H.265 chn0，且 RTSP 线程抢帧（VENC 单消费者） | `sample_venc_rtsp.c` 增加 `SAMPLE_VENC_AS_LIB` + `hisi_pipeline_start/stop`，hisi_push 自建 VI/ISP/VPSS/VENC |
| 3 | 子码流 `venc_chn=1` 建链路失败 | `CHN_NUM_MAX` 被改成 1，`enc_size[1]` 越界写 `vpss_chn_depth` → depth=720 → VPSS `set_chn_attr` 报 `0xa0078007` | `CHN_NUM_MAX` 恢复为 2；VPSS chn0（主通道）保持使能；绑定 VPSS chn → VENC chn |

---

## 三、配置项说明（hisi_push.ini）

```ini
url=http://192.168.100.188:1985/rtc/v1/whip/?app=live&stream=livestream
file=                # 本地 Annex-B h264 文件回放，留空则用 VENC
fps=25               # 文件回放帧率
venc_chn=1           # 0 = 2560x1440 主码流；1 = 720x480 子码流
pipeline=1           # 1 = hisi_push 自建 VI/VPSS/VENC；0 = 使用外部已建通道
gop=15               # IDR 间隔（帧）。30fps 下 15 = 0.5s，30 = 1s
```

注意：url 不要带反引号或引号，板端手工编辑时尤其注意。

---

## 四、其他改动

| 文件 | 改动 |
|---|---|
| `YangHisiPush.cpp` | `evideoCacheNum=4`，发送队列最坏延迟 133ms |
| `main.cpp` | `setvbuf` 行缓冲，解决日志重定向后看不到 |
| `YangHisiVenc.cpp` | 增加 `lag=` 字段：MPP 采集时间 → 送推流的滞留毫秒数 |
| `metaplayer8.pro` | 本地编译去掉 `-lspeexdsp`，改 `-l:libspeexdsp.so.1`（未装 dev 包） |

---

## 五、验证方法

### 1. 板端日志

```sh
grep "venc frame #" /home/hisi_push.log | tail -5
```

正常输出（pts 现为微秒）：

```
venc frame #16800: size=3230 key=0 pts=20071279000 lag=62ms
```

- 每 25 帧 pts 增量约 833000（微秒）= 30fps
- `lag` 稳定几十毫秒说明板端无积压

### 2. SRS 流信息

```sh
curl -s http://192.168.100.188:1985/api/v1/streams/ | python3 -m json.tool
```

期望：`publish.active=true`、`codec=H264`、`width=720 height=480`、`recv_30s` 约 500~700kbps。

### 3. RTP 时间戳正确性（关键验证）

```sh
curl -s -m 12 -o /tmp/live.flv http://192.168.100.188:8080/live/livestream.flv
python3 /tmp/flvstat.py /tmp/live.flv
```

修复前：`381 帧只有 14 个不同 ts`；修复后应为 **每帧 ts 递增约 33ms**。

FLV 统计脚本（无需 ffprobe，本机未安装 ffmpeg 时可用）：

```python
import sys
data = open(sys.argv[1], 'rb').read()
pos = 13
tags = []
while pos + 11 <= len(data):
    t = data[pos]
    ds = int.from_bytes(data[pos+1:pos+4], 'big')
    ts = int.from_bytes(data[pos+4:pos+7], 'big') | (data[pos+7] << 24)
    b = pos + 11
    if t == 9 and b < len(data):
        b0 = data[b]
        avct = data[b+1] if b+1 < len(data) else -1
        if avct == 1:
            tags.append((ts, 'I' if (b0 >> 4) == 1 else 'P'))
    pos = b + ds + 4
print('总帧数', len(tags), '不同ts个数', len(set(t[0] for t in tags)))
print('关键帧ts', [t[0] for t in tags if t[1] == 'I'])
print('前20帧', tags[:20])
```

---

## 六、走过的弯路（排错经验）

1. **误归因于播放器 jitter buffer** —— Chrome `jitterBufferDelay÷emitted≈5ms` 已证明不是播放器
2. **误归因于 GOP / B 帧 / 码率** —— 实测 GOP 为 NORMAL_P 无 B 帧，码率 530kbps 正常
3. **误归因于 VPSS 通道拓扑** —— 实际是数组越界，与通道选择无关
4. **ffprobe 未安装导致误判** —— 多次统计得 0 帧，最后自己解析 FLV 才拿到真实时间戳
5. **Chrome 后台标签节流** —— `327帧/326秒、每帧96KB` 的 stats 疑似后台节流残留

**核心教训**：延迟/卡顿类问题，先抓**时间戳**这个硬指标，比调参数有效得多。

---

## 七、环境注意事项

- `bin/lib_debug` 当前为 **ARM 库**（部署用）；本地 Qt 播放器用的 x64 库在 `bin/lib_debug_x64`
- 若需重新编译本地 metaplayer8，先交换两个目录再编译，结束后换回
- `metaplayer8` 的 `videoPlayCacheNum` 建议 **3**（1 太小易跳帧）
- 部署：`./deploy_hisi.sh`，或板端 NFS 流程 `./start.sh stop` → 复制二进制 → `./start.sh`
- SRS 为 v8.0.74，WHEP 播放端点：`http://<srs>:1985/rtc/v1/whep/?app=live&stream=livestream`
