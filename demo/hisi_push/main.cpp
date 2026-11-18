//
// 海思(Hi3516CV610 等) -> SRS(WHIP) 推流最小示例
//
// 用法:
//   hisi_push [config_file]		默认读取当前目录下的 hisi_push.ini
//
// 配置文件(hisi_push.ini)示例:
//   url=http://192.168.1.100:1985/rtc/v1/whip/?app=live&stream=livestream
//   file=test.h264
//   fps=25
//
//   url  : SRS WHIP 推流地址(必填)
//   file : 本地 Annex-B h264 文件, 留空则等待海思 VENC 送帧
//   fps  : 文件回放帧率, 默认 25
//   venc_chn : 海思 VENC 通道号, >=0 时直接从该通道取码流(需应用已启动 VI/VPSS/VENC)
//
//   帧来源优先级: venc_chn >= 0 > file 非空 > 仅建链等待
//
// 说明:
//   1. 不带 h264 文件时只建立连接，等待外部(海思 VENC)调用 YangHisiPush::sendH264() 送帧。
//   2. 带 h264 文件时按 fps 循环读取 Annex-B 文件并送帧，用于上板前验证链路。
//   3. 板端接入海思 MPP：在 VENC 取流回调里，把每帧 Annex-B 数据交给 sendH264()，
//      keyframe 取 SPS/PPS/IDR 帧，pts 用毫秒时间戳。
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <string>
#include <vector>

#include <yangutil/yangunistd.h>
#include <yangutil/sys/YangLog.h>

#include "YangHisiPush.h"
#include "YangHisiVenc.h"

#ifdef YANG_HISI_PIPELINE
extern "C" int32_t hisi_pipeline_start(int32_t chn, int32_t gop);	// sample_venc_rtsp.c(as lib): 初始化 VI/VPSS/VENC
extern "C" void hisi_pipeline_stop(void);	// 退出时释放编码链路与 ISP
#endif

typedef struct {
	std::string url;
	std::string file;
	int32_t fps;
	int32_t venc_chn;
	int32_t pipeline;	// 1: hisi_push 自己创建 VI/VPSS/VENC; 0: 使用外部已建好的编码通道
	int32_t gop;		// VENC 关键帧间隔(帧数), 默认 30
} PushConfig;

static std::string trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// 简单 key=value 配置解析, 支持 # 和 ; 注释
static int32_t load_config(const char* path, PushConfig& cfg) {
	FILE* fp = fopen(path, "r");
	char line[1024];

	if (fp == NULL)
		return 1;

	cfg.fps = 25;
	cfg.venc_chn = -1;
	cfg.pipeline = 1;
	cfg.gop = 30;
	while (fgets(line, sizeof(line), fp)) {
		std::string s = trim(line);
		if (s.empty() || s[0] == '#' || s[0] == ';')
			continue;

		size_t pos = s.find('=');
		if (pos == std::string::npos)
			continue;

		std::string key = trim(s.substr(0, pos));
		std::string val = trim(s.substr(pos + 1));
		// 去掉包裹的引号(含从聊天/文档复制带来的反引号)
		if (val.size() >= 2 && (val[0] == '"' || val[0] == '\'' || val[0] == '`') && val[val.size() - 1] == val[0])
			val = val.substr(1, val.size() - 2);

		if (key == "url")
			cfg.url = val;
		else if (key == "file")
			cfg.file = val;
		else if (key == "fps")
			cfg.fps = atoi(val.c_str());
		else if (key == "venc_chn")
			cfg.venc_chn = atoi(val.c_str());
		else if (key == "pipeline")
			cfg.pipeline = atoi(val.c_str());
		else if (key == "gop")
			cfg.gop = atoi(val.c_str());
	}
	fclose(fp);

	if (cfg.fps <= 0)
		cfg.fps = 25;
	if (cfg.gop <= 0)
		cfg.gop = 30;
	return Yang_Ok;
}

typedef struct {
	size_t offset;
	size_t len;
	yangbool key;
} H264Frame;

static volatile int g_running = 1;

static void on_signal(int sig) {
	(void) sig;
	g_running = 0;
}

static int32_t find_start_code(const std::vector<uint8_t>& buf, size_t pos, size_t* sc_start, size_t* payload) {
	size_t i = pos;

	while (i + 3 < buf.size()) {
		if (buf[i] == 0 && buf[i + 1] == 0) {
			if (buf[i + 2] == 1) {
				*sc_start = i;
				*payload = i + 3;
				return Yang_Ok;
			}
			if (buf[i + 2] == 0 && i + 3 < buf.size() && buf[i + 3] == 1) {
				*sc_start = i;
				*payload = i + 4;
				return Yang_Ok;
			}
		}
		i++;
	}
	return 1;
}

// 按 Annex-B 起始码切分为访问单元：SPS/PPS 并入其后的 I 帧
static void split_annexb(const std::vector<uint8_t>& buf, std::vector<H264Frame>& frames) {
	size_t sc_start = 0, payload = 0;
	size_t frame_start = (size_t) -1;

	if (find_start_code(buf, 0, &sc_start, &payload) != Yang_Ok)
		return;

	frame_start = sc_start;
	size_t pos = payload;
	yangbool has_vcl = yangfalse;

	while (find_start_code(buf, pos, &sc_start, &payload) == Yang_Ok) {
		int32_t nal_type = buf[payload] & 0x1f;
		pos = payload + 1;

		if (nal_type == 1 || nal_type == 5) {	// 非 IDR / IDR slice
			if (has_vcl) {	// 上一帧结束
				H264Frame f;
				f.offset = frame_start;
				f.len = sc_start - frame_start;
				f.key = yangfalse;
				for (size_t k = f.offset; k + 4 < f.offset + f.len; k++) {
					if (buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 1 && (buf[k + 3] & 0x1f) == 5) {
						f.key = yangtrue;
						break;
					}
				}
				frames.push_back(f);
				frame_start = sc_start;
			}
			has_vcl = yangtrue;
		}
	}

	H264Frame f;
	f.offset = frame_start;
	f.len = buf.size() - frame_start;
	f.key = yangfalse;
	for (size_t k = f.offset; k + 4 < buf.size(); k++) {
		if (buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 1 && (buf[k + 3] & 0x1f) == 5) {
			f.key = yangtrue;
			break;
		}
	}
	frames.push_back(f);
}

static int32_t load_file(const char* path, std::vector<uint8_t>& buf) {
	FILE* fp = fopen(path, "rb");
	if (fp == NULL)
		return 1;

	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (len <= 0) {
		fclose(fp);
		return 1;
	}

	buf.resize((size_t) len);
	size_t rd = fread(buf.data(), 1, buf.size(), fp);
	fclose(fp);
	return rd == buf.size() ? Yang_Ok : 1;
}

int main(int argc, char** argv) {
	// 输出重定向到文件时 stdout 默认全缓冲, 日志会延迟到进程退出才落盘
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char* cfg_path = argc > 1 ? argv[1] : "hisi_push.ini";

	PushConfig cfg;
	if (load_config(cfg_path, cfg)) {
		printf("read config file(%s) failure\n", cfg_path);
		printf("usage: %s [config_file]\n", argv[0]);
		return 1;
	}
	if (cfg.url.empty()) {
		printf("config error: url is empty(%s)\n", cfg_path);
		return 1;
	}

	printf("config: %s\n", cfg_path);
	printf("  url  = %s\n", cfg.url.c_str());
	printf("  file = %s\n", cfg.file.empty() ? "(none, wait for venc)" : cfg.file.c_str());
	printf("  fps  = %d\n", cfg.fps);
	printf("  venc_chn = %d\n", cfg.venc_chn);
	printf("  pipeline = %d\n", cfg.pipeline);
	printf("  gop = %d\n", cfg.gop);

#ifdef YANG_HISI_PIPELINE
	if (cfg.venc_chn >= 0 && cfg.pipeline) {
		// 自己创建 VI/VPSS/VENC, 不再依赖 sample_venc_rtsp
		printf("init hisi vi/vpss/venc pipeline ...\n");
		fflush(stdout);
		if (hisi_pipeline_start(cfg.venc_chn, cfg.gop) != 0) {
			printf("hisi_pipeline_start failed(若编码通道已由其他程序创建, 请把 pipeline 设为 0)\n");
			return 1;
		}
	}
#endif

	const char* url = cfg.url.c_str();
	const char* file = cfg.file.empty() ? NULL : cfg.file.c_str();
	int32_t fps = cfg.fps;

	std::vector<uint8_t> h264;
	std::vector<H264Frame> frames;
	if (file) {
		if (load_file(file, h264)) {
			printf("read h264 file(%s) failure\n", file);
			return 1;
		}
		split_annexb(h264, frames);
		if (frames.empty()) {
			printf("no h264 frame found in %s (need Annex-B)\n", file);
			return 1;
		}
		printf("loaded %s: %zu bytes, %zu frames\n", file, h264.size(), frames.size());
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	YangHisiPush push;
	if (push.init(url)) {
		printf("init push failure, url=%s\n", url);
		return 1;
	}
	if (push.start()) {
		printf("start push failure\n");
		return 1;
	}
	printf("connecting srs ...\n");

	int32_t waited = 0;
	while (g_running && !push.isPublished() && waited < 10000) {
		yang_usleep(100 * 1000);
		waited += 100;
	}
	if (!push.isPublished()) {
		printf("connect srs failure(timeout)\n");
		push.stop();
#ifdef YANG_HISI_PIPELINE
		hisi_pipeline_stop();
#endif
		return 1;
	}
	printf("connected, start sending\n");

	if (cfg.venc_chn >= 0) {
		YangHisiVenc venc;
		if (venc.start(cfg.venc_chn, &push)) {
			printf("start venc capture(chn=%d) failure\n", cfg.venc_chn);
			push.stop();
			return 1;
		}
		printf("venc capture chn=%d started, ctrl+c to stop\n", cfg.venc_chn);
		while (g_running)
			yang_usleep(200 * 1000);
		venc.stop();
		push.stop();
#ifdef YANG_HISI_PIPELINE
		hisi_pipeline_stop();
#endif
		return 0;
	}

	if (file == NULL) {
		printf("no input file, waiting for sendH264() from VENC ...\n");
		while (g_running)
			yang_usleep(200 * 1000);
		push.stop();
		return 0;
	}

	int64_t frame_count = 0;
	while (g_running) {
		for (size_t i = 0; i < frames.size() && g_running; i++) {
			const H264Frame& f = frames[i];
			int64_t pts = frame_count * 1000000 / fps;	// metaRTC 时间戳使用微秒
			push.sendH264(h264.data() + f.offset, (int32_t) f.len, pts, f.key);
			frame_count++;
			yang_usleep(1000 * 1000 / fps);
		}
	}

	printf("stop push, sent %lld frames\n", (long long) frame_count);
	push.stop();
	return 0;
}
