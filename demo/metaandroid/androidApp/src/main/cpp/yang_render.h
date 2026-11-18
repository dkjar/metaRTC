#ifndef META_ANDROID_YANG_RENDER_H_
#define META_ANDROID_YANG_RENDER_H_

#include <android/native_window.h>
#include <stdint.h>

// 把解码后的 I420 帧直接画到 SurfaceView 的 ANativeWindow 上
class YangAndroidRender {
public:
	YangAndroidRender();
	~YangAndroidRender();

	// win 由 ANativeWindow_fromSurface 获得, 内部接管其引用计数, 传 NULL 表示释放
	void setWindow(ANativeWindow *win);
	void drawI420(int32_t width, int32_t height, const uint8_t *data, int32_t len);

private:
	ANativeWindow *m_win;
	int32_t m_width;
	int32_t m_height;
};

#endif
