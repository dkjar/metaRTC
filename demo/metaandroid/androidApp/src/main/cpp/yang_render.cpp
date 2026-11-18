#include "yang_render.h"

#include <stdlib.h>
#include <string.h>

YangAndroidRender::YangAndroidRender() {
	m_win = NULL;
	m_width = 0;
	m_height = 0;
}

YangAndroidRender::~YangAndroidRender() {
	setWindow(NULL);
}

void YangAndroidRender::setWindow(ANativeWindow *win) {
	if (m_win != NULL) {
		ANativeWindow_release(m_win);
		m_win = NULL;
	}
	m_win = win;
	m_width = 0;
	m_height = 0;
}

static inline int32_t clamp255(int32_t v) {
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void YangAndroidRender::drawI420(int32_t width, int32_t height, const uint8_t *data, int32_t len) {
	if (m_win == NULL || data == NULL || width <= 0 || height <= 0)
		return;
	if (len < width * height * 3 / 2)
		return;

	if (m_width != width || m_height != height) {
		if (ANativeWindow_setBuffersGeometry(m_win, width, height, WINDOW_FORMAT_RGBA_8888) != 0)
			return;
		m_width = width;
		m_height = height;
	}

	ANativeWindow_Buffer buf;
	if (ANativeWindow_lock(m_win, &buf, NULL) != 0)
		return;

	uint8_t *out = (uint8_t *) buf.bits;
	if (out == NULL) {
		ANativeWindow_unlockAndPost(m_win);
		return;
	}

	const uint8_t *yp = data;
	const uint8_t *up = data + width * height;
	const uint8_t *vp = up + width * height / 4;
	int32_t cw = width / 2;

	for (int32_t j = 0; j < height && j < buf.height; j++) {
		uint32_t *row = (uint32_t *) (out + (size_t) j * buf.stride * 4);
		const uint8_t *yrow = yp + (size_t) j * width;
		const uint8_t *urow = up + (size_t) (j / 2) * cw;
		const uint8_t *vrow = vp + (size_t) (j / 2) * cw;

		for (int32_t i = 0; i < width && i < buf.width; i++) {
			int32_t y = yrow[i];
			int32_t u = (int32_t) urow[i / 2] - 128;
			int32_t v = (int32_t) vrow[i / 2] - 128;

			int32_t r = clamp255(y + ((359 * v) >> 8));
			int32_t g = clamp255(y - ((88 * u + 183 * v) >> 8));
			int32_t b = clamp255(y + ((454 * u) >> 8));

			row[i] = 0xFF000000u | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
		}
	}

	ANativeWindow_unlockAndPost(m_win);
}
