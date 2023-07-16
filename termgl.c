/*
 * Copyright (c) 2021-2023 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 **/

#include "termgl.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Setting MACROs
 */
#define TGL_CLEAR_SCR                              \
	do {                                       \
		fputs("\033[1;1H\033[2J", stdout); \
	} while (0)
#define TGL_TYPEOF __typeof__
#define TGL_MALLOC malloc
#define TGL_FREE free
#define TGL_LIKELY(x_) __builtin_expect((x_), 1)
#define TGL_UNLIKELY(x_) __builtin_expect((x_), 0)
#define TGL_UNREACHABLE() __builtin_unreachable()

#ifdef TGL_OS_WINDOWS
#include <windows.h>
#define WINDOWS_CALL(cond_, retval_)            \
	do {                                    \
		if (TGL_UNLIKELY(cond_)) {      \
			errno = GetLastError(); \
			return retval_;         \
		}                               \
	} while (0)
#endif

typedef struct Pixel {
	char v_char;
	uint16_t color;
} Pixel;

struct TGL {
	unsigned width;
	unsigned height;
	int max_x;
	int max_y;
	unsigned frame_size;
	Pixel *frame_buffer;
	float *z_buffer;
	char *output_buffer;
	unsigned output_buffer_size;
	bool z_buffer_enabled;
	uint8_t settings;
};

#define SWAP(a_, b_)           \
	do {                   \
		TGL_TYPEOF(a_) \
		temp_ = a_;    \
		a_ = b_;       \
		b_ = temp_;    \
	} while (0)
#define MIN(a_, b_) (((a_) < (b_)) ? (a_) : (b_))
#define MAX(a_, b_) (((a_) > (b_)) ? (a_) : (b_))
#define XOR(a_, b_) (((bool)(a_)) != ((bool)(b_)))

#define SET_PIXEL_RAW(tgl_, x_, y_, v_char_, color_)                     \
	do {                                                             \
		*(&tgl_->frame_buffer[y_ * tgl_->width + x_]) = (Pixel){ \
			.v_char = v_char_,                               \
			.color = color_,                                 \
		};                                                       \
	} while (0)

#define SET_PIXEL(tgl_, x_, y_, z_, v_char_, color_)                      \
	do {                                                              \
		if (!tgl_->z_buffer_enabled) {                            \
			SET_PIXEL_RAW(tgl_, x_, y_, v_char_, color_);     \
		} else if (z_ >= tgl_->z_buffer[y_ * tgl_->width + x_]) { \
			SET_PIXEL_RAW(tgl_, x_, y_, v_char_, color_);     \
			tgl->z_buffer[y_ * tgl_->width + x_] = z_;        \
		}                                                         \
	} while (0)

#define CALL(stmt_, retval_)             \
	do {                             \
		if (TGL_UNLIKELY(stmt_)) \
			return retval_;  \
	} while (0)
#define CALL_STDOUT(stmt_, retval_) CALL((stmt_) == EOF, retval_)

const TGLGradient gradient_full = {
	.length = 70,
	.grad = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$",
};

const TGLGradient gradient_min = {
	.length = 10,
	.grad = " .:-=+*#%@",
};

static void itgl_clip(const TGL *tgl, int *x, int *y);
static char *itgl_generate_sgr(uint16_t color_prev, uint16_t color_cur, char *buf);
static void itgl_horiz_line(TGL *tgl, int x0, float z0, uint8_t u0, uint8_t v0, int x1, float z1, uint8_t u1, uint8_t v1, int y, TGLInterp *t, const void *data);

char tgl_grad_char(const TGLGradient *const grad, const uint8_t intensity)
{
	return grad->grad[grad->length * intensity / 256u];
}

inline void itgl_clip(const TGL *const tgl, int *const x, int *const y)
{
	*x = MAX(MIN(tgl->max_x, *x), 0);
	*y = MAX(MIN(tgl->max_y, *y), 0);
}

void tgl_clear(TGL *const tgl, const uint8_t buffers)
{
	unsigned i;
	if (buffers & TGL_FRAME_BUFFER) {
		for (i = 0; i < tgl->frame_size; i++) {
			*(&tgl->frame_buffer[i]) = (Pixel){
				.v_char = ' ',
				.color = 0x0000,
			};
		}
	}
	if (buffers & TGL_Z_BUFFER)
		for (i = 0; i < tgl->frame_size; i++)
			tgl->z_buffer[i] = -1.f;
	if (buffers & TGL_OUTPUT_BUFFER)
		memset(tgl->output_buffer, '\0', tgl->output_buffer_size);
}

void tgl_clear_screen(void)
{
	TGL_CLEAR_SCR;
}

TGL *tgl_init(const unsigned width, const unsigned height)
{
#ifdef TGL_OS_WINDOWS
	static bool win_init = false;
	if (!win_init) {
		win_init = true;

		const HANDLE hOutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
		WINDOWS_CALL(hOutputHandle == INVALID_HANDLE_VALUE, NULL);
		DWORD mode;
		WINDOWS_CALL(!GetConsoleMode(hOutputHandle, &mode), NULL);
		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		WINDOWS_CALL(!SetConsoleMode(hOutputHandle, mode), NULL);

		const HANDLE hInputHandle = GetStdHandle(STD_INPUT_HANDLE);
		WINDOWS_CALL(hInputHandle == INVALID_HANDLE_VALUE, NULL);
		WINDOWS_CALL(!GetConsoleMode(hInputHandle, &mode), NULL);
		mode &= ~(ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
		WINDOWS_CALL(!SetConsoleMode(hInputHandle, mode), NULL);
	}
#endif
	TGL *const tgl = TGL_MALLOC(sizeof(TGL));
	if (!tgl)
		return NULL;
	*tgl = (TGL){
		.width = width,
		.height = height,
		.max_x = width - 1,
		.max_y = height - 1,
		.frame_size = width * height,
		.frame_buffer = TGL_MALLOC(sizeof(Pixel) * width * height),
	};
	if (!tgl->frame_buffer) {
		TGL_FREE(tgl);
		return NULL;
	}
	tgl_clear(tgl, TGL_FRAME_BUFFER);
	return tgl;
}

char *itgl_generate_sgr(const uint16_t color_prev, const uint16_t color_cur, char *buf)
{
	const uint16_t enable = color_cur & ~color_prev;
	const uint16_t disable = color_prev & ~color_cur;
	const uint16_t xor = color_cur ^ color_prev;
	bool flag_delim = false;

	*buf++ = '\033';
	*buf++ = '[';

	if (disable & TGL_BOLD) {
		*buf++ = '2';
		*buf++ = '2';
		flag_delim = true;
	} else if (enable & TGL_BOLD) {
		*buf++ = '1';
		flag_delim = true;
	}

	if (disable & TGL_UNDERLINE) {
		if (flag_delim)
			*buf++ = ';';
		else
			flag_delim = true;
		*buf++ = '2';
		*buf++ = '4';
	} else if (enable & TGL_UNDERLINE) {
		if (flag_delim)
			*buf++ = ';';
		else
			flag_delim = true;
		*buf++ = '4';
	}

	if (xor&0x000F) {
		if (flag_delim)
			*buf++ = ';';
		else
			flag_delim = true;
		*buf++ = (color_cur & TGL_HIGH_INTENSITY) ? '9' : '3';
		*buf++ = (color_cur & 0x0007) + 48;
	}

	if (xor&0x00F0) {
		if (flag_delim)
			*buf++ = ';';
		if (color_cur & TGL_HIGH_INTENSITY_BKG) {
			*buf++ = '1';
			*buf++ = '0';
		} else {
			*buf++ = '4';
		}
		*buf++ = ((color_cur & 0x0070) >> 4) + 48;
	}
	*buf++ = 'm';

	return buf;
}

int tgl_flush(TGL *const tgl)
{
	if (tgl->settings & TGL_PROGRESSIVE)
		CALL_STDOUT(fputs("\033[;H", stdout), -1);
	else
		TGL_CLEAR_SCR;

	uint16_t color = 0x0007;
	unsigned row, col;
	Pixel *pixel = tgl->frame_buffer;
	const bool double_chars = tgl->settings & TGL_DOUBLE_CHARS;

	if (tgl->output_buffer_size) {
		char *output_buffer_loc = tgl->output_buffer;
		for (row = 0; row < tgl->height; row++) {
			for (col = 0; col < tgl->width; col++) {
				if (color != pixel->color) {
					output_buffer_loc = itgl_generate_sgr(color, pixel->color, output_buffer_loc);
					color = pixel->color;
				}
				*output_buffer_loc++ = pixel->v_char;
				if (double_chars)
					*output_buffer_loc++ = pixel->v_char;
				pixel++;
			}
			*output_buffer_loc++ = '\n';
		}
		*output_buffer_loc++ = '\033';
		*output_buffer_loc++ = '[';
		*output_buffer_loc++ = '0';
		*output_buffer_loc = 'm';
		CALL_STDOUT(fputs(tgl->output_buffer, stdout), -1);
	} else {
		for (row = 0; row < tgl->height; row++) {
			for (col = 0; col < tgl->width; col++) {
				if (color != pixel->color) {
					char buf[16];
					*itgl_generate_sgr(color, pixel->color, buf) = '\0';
					color = pixel->color;
					CALL_STDOUT(fputs(buf, stdout), -1);
				}
				CALL_STDOUT(putchar(pixel->v_char), -1);
				if (double_chars)
					CALL_STDOUT(putchar(pixel->v_char), -1);
				pixel++;
			}
			CALL_STDOUT(putchar('\n'), -1);
		}
		CALL_STDOUT(fputs("\033[0m", stdout), -1);
	}

	CALL_STDOUT(fflush(stdout), -1);
	return 0;
}

void tgl_putchar(TGL *const tgl, int x, int y, const char c, const uint16_t color)
{
	itgl_clip(tgl, &x, &y);
	SET_PIXEL_RAW(tgl, x, y, c, color);
}

void tgl_puts(TGL *const tgl, const int x, int y, const char *str, const uint16_t color)
{
	int cur_x = x;
	while (*str) {
		if (TGL_UNLIKELY(*str == '\n')) {
			cur_x = x;
			y++;
			str++;
			continue;
		}
		itgl_clip(tgl, &cur_x, &y);
		SET_PIXEL_RAW(tgl, cur_x, y, *str, color);
		cur_x++;
		str++;
	}
}

void tgl_point(TGL *const tgl, int x, int y, const float z, const char c, const uint16_t color)
{
	itgl_clip(tgl, &x, &y);
	SET_PIXEL(tgl, x, y, z, c, color);
}

/* Bresenham's line algorithm */
void tgl_line(TGL *const tgl, int x0, int y0, float z0, uint8_t u0, uint8_t v0, int x1, int y1, float z1, uint8_t u1, uint8_t v1, TGLInterp *const t, const void *const data)
{
	itgl_clip(tgl, &x0, &y0);
	itgl_clip(tgl, &x1, &y1);
	if (abs(y1 - y0) < abs(x1 - x0)) {
		if (x0 > x1) {
			SWAP(x1, x0);
			SWAP(y1, y0);
			SWAP(z1, z0);
			SWAP(u1, u0);
			SWAP(v1, v0);
		}
		int dx = x1 - x0;
		int dy = y1 - y0;
		int yi;
		if (dy > 0) {
			yi = 1;
		} else {
			yi = -1;
			dy *= -1;
		}
		int d = (dy + dy) - dx;
		int y = y0;
		int x;
		for (x = x0; x <= x1; x++) {
			uint16_t color;
			char c;
			t(((x - x0) * u1 + (x1 - x) * u0) / dx, ((x - x0) * v1 + (x1 - x) * v0) / dx, &color, &c, data);
			SET_PIXEL(tgl, x, y,
				((x - x0) * z1 + (x1 - x) * z0) / dx,
				c, color);
			if (d > 0) {
				y += yi;
				d += 2 * (dy - dx);
			} else {
				d += dy + dy;
			}
		}
	} else {
		if (y0 > y1) {
			SWAP(x1, x0);
			SWAP(y1, y0);
			SWAP(z1, z0);
			SWAP(u1, u0);
			SWAP(v1, v0);
		}
		int dx = x1 - x0;
		int dy = y1 - y0;
		int xi;
		if (dx > 0) {
			xi = 1;
		} else {
			xi = -1;
			dx *= -1;
		}
		int d = (dx + dx) - dy;
		int x = x0;
		int y;
		for (y = y0; y <= y1; y++) {
			uint16_t color;
			char c;
			t(((y - y0) * u1 + (y1 - y) * u0) / dy, ((y - y0) * v1 + (y1 - y) * v0) / dy, &color, &c, data);
			SET_PIXEL(tgl, x, y,
				((y - y0) * z1 + (y1 - y) * z0) / dx,
				c, color);
			if (d > 0) {
				x += xi;
				d += 2 * (dx - dy);
			} else {
				d += dx + dx;
			}
		}
	}
}

void tgl_interp_lin_1d(const uint8_t u, const uint8_t v, uint16_t *const color, char *const c, const void *const data)
{
	const TGLInterpLin1D *const interp = data;
	*color = interp->color;
	*c = tgl_grad_char(interp->grad, (interp->u0 * u + interp->u1 * (255 - u)) / 256);
	(void)v;
}

void tgl_interp_lin_2d(const uint8_t u, const uint8_t v, uint16_t *const color, char *const c, const void *const data)
{
	const TGLInterpLin2D *const interp = data;
	*color = interp->color;
	*c = tgl_grad_char(interp->grad, (interp->uv0 + (interp->u1 - interp->uv0) * u + (interp->v1 - interp->uv0) * v) / 256);
}

void tgl_triangle(TGL *const tgl, const int x0, const int y0, const float z0, const uint8_t u0, const uint8_t v0, const int x1, const int y1, const float z1, const uint8_t u1, const int x2, const int y2, const float z2, const uint8_t v1, TGLInterp *const t, const void *data)
{
	tgl_line(tgl, x0, y0, z0, u0, v0, x1, y1, z1, u1, v0, t, data);
	tgl_line(tgl, x0, y0, z0, u0, v0, x2, y2, z2, u0, v1, t, data);
	tgl_line(tgl, x1, y1, z1, u1, v0, x2, y2, z2, u0, v1, t, data);
}

void itgl_horiz_line(TGL *const tgl, const int x0, const float z0, const uint8_t u0, const uint8_t v0, const int x1, const float z1, const uint8_t u1, const uint8_t v1, const int y, TGLInterp *t, const void *const data)
{
	if (x0 == x1) {
		char c;
		uint16_t color;
		t(u0, v0, &color, &c, data);
		SET_PIXEL(tgl, x0, y, z0, c, color);
	} else {
		const int dx = x1 - x0;
		int x;
		for (x = x0; x <= x1; x++) {
			char c;
			uint16_t color;
			t(((x - x0) * u1 + (x1 - x) * u0) / dx, ((x - x0) * v1 + (x1 - x) * v0) / dx, &color, &c, data);
			SET_PIXEL(tgl, x, y,
				((x - x0) * z1 + (x1 - x) * z0) / dx,
				c, color);
		}
	}
}

/* Solution based on Bresenham's line algorithm
 * adapted from: https://github.com/OneLoneCoder/videos/blob/master/olcConsoleGameEngine.h
 **/
void tgl_triangle_fill(TGL *const tgl, int x0, int y0, float z0, const uint8_t u0, const uint8_t v0, int x1, int y1, float z1, uint8_t u1, int x2, int y2, float z2, const uint8_t v1, TGLInterp *const t, const void *data)
{
	uint8_t verts_u[3] = { u0, u1, u0 };
	uint8_t verts_v[3] = { v0, v0, v1 };
	itgl_clip(tgl, &x0, &y0);
	itgl_clip(tgl, &x1, &y1);
	itgl_clip(tgl, &x2, &y2);
	if (y1 < y0) {
		SWAP(x1, x0);
		SWAP(y1, y0);
		SWAP(z1, z0);
		SWAP(verts_u[1], verts_u[0]);
		SWAP(verts_v[1], verts_v[0]);
	}
	if (y2 < y0) {
		SWAP(x2, x0);
		SWAP(y2, y0);
		SWAP(z2, z0);
		SWAP(verts_u[2], verts_u[0]);
		SWAP(verts_v[2], verts_v[0]);
	}
	if (y2 < y1) {
		SWAP(x1, x2);
		SWAP(y1, y2);
		SWAP(z1, z2);
		SWAP(verts_u[1], verts_u[2]);
		SWAP(verts_v[1], verts_v[2]);
	}

	int t0xp, t1xp, minx, maxx, t0x, t1x;
	t0x = t1x = x0;
	int y = y0;
	int dx0 = x1 - x0;
	int signx0, signx1;
	bool changed0, changed1;
	changed0 = changed1 = false;
	if (dx0 < 0) {
		dx0 *= -1;
		signx0 = -1;
	} else {
		signx0 = 1;
	}
	int dy0 = y1 - y0;
	int dx1 = x2 - x0;
	if (dx1 < 0) {
		dx1 = -dx1;
		signx1 = -1;
	} else {
		signx1 = 1;
	}
	int dy1 = y2 - y0;

	if (dy0 > dx0) {
		SWAP(dx0, dy0);
		changed0 = true;
	}
	if (dy1 > dx1) {
		SWAP(dx1, dy1);
		changed1 = true;
	}
	int e1 = dx1 >> 1;
	int e0;
	if (y0 == y1)
		goto LBL_NEXT;
	e0 = dx0 >> 1;

	for (int i = 0; i < dx0;) {
		t0xp = t1xp = 0;
		if (t0x < t1x) {
			minx = t0x;
			maxx = t1x;
		} else {
			minx = t1x;
			maxx = t0x;
		}
		while (i < dx0) {
			i++;
			e0 += dy0;
			while (e0 >= dx0) {
				e0 -= dx0;
				if (changed0)
					t0xp = signx0;
				else
					goto LBL_NEXT1;
			}
			if (changed0)
				break;
			else
				t0x += signx0;
		}
		/* Move line */
	LBL_NEXT1:
		/* process second line until y value is about to change */
		while (true) {
			e1 += dy1;
			while (e1 >= dx1) {
				e1 -= dx1;
				if (changed1)
					t1xp = signx1;
				else
					goto LBL_NEXT2;
			}
			if (changed1)
				break;
			else
				t1x += signx1;
		}
	LBL_NEXT2:
		if (minx > t0x)
			minx = t0x;
		if (minx > t1x)
			minx = t1x;
		if (maxx < t0x)
			maxx = t0x;
		if (maxx < t1x)
			maxx = t1x;

		uint8_t u0 = ((y - y0) * verts_u[1] + (y1 - y) * verts_u[0]) / (y1 - y0);
		uint8_t v0 = ((y - y0) * verts_v[1] + (y1 - y) * verts_v[0]) / (y1 - y0);
		uint8_t u1 = ((y - y0) * verts_u[2] + (y2 - y) * verts_u[0]) / (y2 - y0);
		uint8_t v1 = ((y - y0) * verts_v[2] + (y2 - y) * verts_v[0]) / (y2 - y0);
		float vz0 = ((y - y0) * z1 + (y1 - y) * z0) / (y1 - y0);
		float vz1 = ((y - y0) * z1 + (y1 - y) * z0) / (y1 - y0);

		if (t0x < t1x)
			itgl_horiz_line(tgl, minx, vz0, u0, v0, maxx, vz1, u1, v1, y, t, data);
		else
			itgl_horiz_line(tgl, minx, vz1, u1, v1, maxx, vz0, u0, v0, y, t, data);

		if (!changed0)
			t0x += signx0;
		t0x += t0xp;
		if (!changed1)
			t1x += signx1;
		t1x += t1xp;
		y += 1;
		if (y == y1)
			break;
	}
LBL_NEXT:
	/* Second half */
	dx0 = x2 - x1;
	if (dx0 < 0) {
		dx0 *= -1;
		signx0 = -1;
	} else {
		signx0 = 1;
	}
	dy0 = y2 - y1;
	t0x = x1;
	if (dy0 > dx0) {
		SWAP(dy0, dx0);
		changed0 = true;
	} else {
		changed0 = false;
	}
	e0 = dx0 >> 1;

	for (int i = 0; i <= dx0; i++) {
		t0xp = t1xp = 0;
		if (t0x < t1x) {
			minx = t0x;
			maxx = t1x;
		} else {
			minx = t1x;
			maxx = t0x;
		}
		while (i < dx0) {
			e0 += dy0;
			while (e0 >= dx0) {
				e0 -= dx0;
				if (changed0) {
					t0xp = signx0;
					break;
				} else {
					goto LBL_NEXT3;
				}
			}
			if (changed0)
				break;
			else
				t0x += signx0;
			if (i < dx0)
				i++;
		}
	LBL_NEXT3:
		while (t1x != x2) {
			e1 += dy1;
			while (e1 >= dx1) {
				e1 -= dx1;
				if (changed1)
					t1xp = signx1;
				else
					goto LBL_NEXT4;
			}
			if (changed1)
				break;
			else
				t1x += signx1;
		}
	LBL_NEXT4:
		if (minx > t0x)
			minx = t0x;
		if (minx > t1x)
			minx = t1x;
		if (maxx < t0x)
			maxx = t0x;
		if (maxx < t1x)
			maxx = t1x;

		if (y1 != y2) {
			uint8_t u0 = ((y - y0) * verts_u[2] + (y2 - y) * verts_u[0]) / (y2 - y0);
			uint8_t v0 = ((y - y0) * verts_v[2] + (y2 - y) * verts_v[0]) / (y2 - y0);
			uint8_t u1 = ((y - y1) * verts_u[2] + (y2 - y) * verts_u[1]) / (y2 - y1);
			uint8_t v1 = ((y - y1) * verts_v[2] + (y2 - y) * verts_v[1]) / (y2 - y1);
			float vz0 = ((y - y0) * z2 + (y2 - y) * z0) / (y2 - y0);
			float vz1 = ((y - y1) * z2 + (y2 - y) * z1) / (y2 - y1);

			if (t1x < t0x)
				itgl_horiz_line(tgl, minx, vz0, u0, v0, maxx, vz1, u1, v1, y, t, data);
			else
				itgl_horiz_line(tgl, minx, vz1, u1, v1, maxx, vz0, u0, v0, y, t, data);
		} else {
			itgl_horiz_line(tgl, minx, z1, verts_u[1], verts_v[1], maxx, z2, verts_u[2], verts_v[2], y, t, data);
		}

		if (!changed0)
			t0x += signx0;
		t0x += t0xp;
		if (!changed1)
			t1x += signx1;
		t1x += t1xp;
		y += 1;
		if (y > y2)
			return;
	};
}

int tgl_enable(TGL *const tgl, const uint8_t settings)
{
	const uint8_t enable = settings & ~tgl->settings;
	tgl->settings |= settings;
	if (enable & TGL_Z_BUFFER) {
		tgl->z_buffer_enabled = true;
		tgl->z_buffer = TGL_MALLOC(sizeof(float) * tgl->frame_size);
		if (!tgl->z_buffer)
			return -1;
		tgl_clear(tgl, TGL_Z_BUFFER);
	}
	if (enable & TGL_OUTPUT_BUFFER) {
		/* Longest SGR code: \033[22;24;XX;10Xm (length 15)
		 * Maximum 16 chars per pixel: SGR + 2 x char
		 * 1 Newline character per line
		 * 1 NUL terminator
		 * SGR clear code: \033[0m (length 4)
		 */
		tgl->output_buffer_size = 17u * tgl->frame_size + tgl->height + 4u + 1u;
		tgl->output_buffer = TGL_MALLOC(tgl->output_buffer_size);
		if (!tgl->output_buffer)
			return -1;
		tgl_clear(tgl, TGL_OUTPUT_BUFFER);
	}
	return 0;
}

void tgl_disable(TGL *const tgl, const uint8_t settings)
{
	tgl->settings &= ~settings;
	if (settings & TGL_Z_BUFFER) {
		tgl->z_buffer_enabled = false;
		TGL_FREE(tgl->z_buffer);
		tgl->z_buffer = NULL;
	}
	if (settings & TGL_OUTPUT_BUFFER) {
		tgl->output_buffer_size = 0;
		TGL_FREE(tgl->output_buffer);
		tgl->output_buffer = NULL;
	}
}

void tgl_delete(TGL *const tgl)
{
	TGL_FREE(tgl->frame_buffer);
	TGL_FREE(tgl->z_buffer);
	TGL_FREE(tgl->output_buffer);
	TGL_FREE(tgl);
}

#ifdef TERMGL3D

#include <math.h>

#define TGL_CULL_FACE_BIT 0x01
#define TGL_WINDING_BIT 0x02

#define MAP_COORD(half_, val_) ((val_ * half_) + half_)

enum /* clip planes */ {
	CLIP_NEAR = 0,
	CLIP_FAR,
	CLIP_LEFT,
	CLIP_RIGHT,
	CLIP_TOP,
	CLIP_BOTTOM,
};

const TGLVec3 clip_plane_normals[6] = {
	[CLIP_NEAR] = { 0.f, 0.f, -1.f },
	[CLIP_FAR] = { 0.f, 0.f, 1.f },
	[CLIP_LEFT] = { 1.f, 0.f, 0.f },
	[CLIP_RIGHT] = { -1.f, 0.f, 0.f },
	[CLIP_BOTTOM] = { 0.f, 1.f, 0.f },
	[CLIP_TOP] = { 0.f, -1.f, 0.f },
};

static float itgl_distance_point_plane(const TGLVec3 normal, const TGLVec4 point);
static float itgl_line_intersect_plane(const TGLVec3 normal, const TGLVec3 start, const TGLVec3 end, TGLVec3 point);
static unsigned itgl_clip_triangle_plane(const TGLVec3 normal, const TGLVec4 in[3], TGLVec4 out[2][3]);

__attribute__((const)) float tgl_sqr(const float val)
{
	return val * val;
}

__attribute__((const)) float tgl_mag3(const float vec[3])
{
	return sqrtf(tgl_sqr(vec[0]) + tgl_sqr(vec[1]) + tgl_sqr(vec[2]));
}

__attribute__((const)) float tgl_magsqr3(const float vec[3])
{
	return tgl_sqr(vec[0]) + tgl_sqr(vec[1]) + tgl_sqr(vec[2]);
}

__attribute__((const)) float tgl_dot3(const float vec1[3], const float vec2[3])
{
	return vec1[0] * vec2[0] + vec1[1] * vec2[1] + vec1[2] * vec2[2];
}

__attribute__((const)) float tgl_dot43(const float vec1[4], const float vec2[3])
{
	return vec1[0] * vec2[0] + vec1[1] * vec2[1] + vec1[2] * vec2[2] + vec1[3];
}

void tgl_add3s(const float vec1[3], const float summand, float res[3])
{
	res[0] = vec1[0] + summand;
	res[1] = vec1[1] + summand;
	res[2] = vec1[2] + summand;
}

void tgl_sub3s(const float vec1[3], const float subtrahend, float res[3])
{
	res[0] = vec1[0] - subtrahend;
	res[1] = vec1[1] - subtrahend;
	res[2] = vec1[2] - subtrahend;
}

void tgl_mul3s(const float vec[3], const float mul, float res[3])
{
	res[0] = vec[0] * mul;
	res[1] = vec[1] * mul;
	res[2] = vec[2] * mul;
}

void tgl_add3v(const float vec1[3], const float vec2[3], float res[3])
{
	res[0] = vec1[0] + vec2[0];
	res[1] = vec1[1] + vec2[1];
	res[2] = vec1[2] + vec2[2];
}

void tgl_sub3v(const float vec1[3], const float vec2[3], float res[3])
{
	res[0] = vec1[0] - vec2[0];
	res[1] = vec1[1] - vec2[1];
	res[2] = vec1[2] - vec2[2];
}

void tgl_mul3v(const float vec1[3], const float vec2[3], float res[3])
{
	res[0] = vec1[0] * vec2[0];
	res[1] = vec1[1] * vec2[1];
	res[2] = vec1[2] * vec2[2];
}

void tgl_inv3(const float vec[3], float res[3])
{
	res[0] = 1.f / vec[0];
	res[1] = 1.f / vec[1];
	res[2] = 1.f / vec[2];
}

void tgl_cross(const float vec1[3], const float vec2[3], float res[3])
{
	res[0] = vec1[1] * vec2[2] - vec1[2] * vec2[1];
	res[1] = vec1[2] * vec2[0] - vec1[0] * vec2[2];
	res[2] = vec1[0] * vec2[1] - vec1[1] * vec2[0];
}

void tgl_norm3(float vec[3])
{
	tgl_mul3s(vec, 1.f / tgl_mag3(vec), vec);
}

void tgl_mulmatvec(TGLMat mat, const TGLVec3 vec, TGLVec4 res)
{
	res[0] = tgl_dot43(mat[0], vec);
	res[1] = tgl_dot43(mat[1], vec);
	res[2] = tgl_dot43(mat[2], vec);
	res[3] = tgl_dot43(mat[3], vec);
}

void tgl_mulmatvec3(TGLMat mat, const TGLVec3 vec, TGLVec3 res)
{
	res[0] = tgl_dot43(mat[0], vec);
	res[1] = tgl_dot43(mat[1], vec);
	res[2] = tgl_dot43(mat[2], vec);

	float w = tgl_dot43(mat[3], vec);
	if (w != 0.f) {
		w = 1.f / w;
		res[0] *= w;
		res[1] *= w;
		res[2] *= w;
	}
}

void tgl_mulmat(TGLMat mat1, TGLMat mat2, TGLMat res)
{
	unsigned c, d, k;
#pragma GCC unroll 4
	for (c = 0; c < 4u; c++)
#pragma GCC unroll 4
		for (d = 0; d < 4u; d++) {
			res[c][d] = 0.f;
#pragma GCC unroll 4
			for (k = 0; k < 4u; k++)
				res[c][d] += mat1[c][k] * mat2[k][d];
		}
}

__attribute__((const)) float itgl_distance_point_plane(const TGLVec3 normal, const TGLVec4 point)
{
	return tgl_dot3(normal, point) + point[3];
}

void tgl3d_camera(TGLMat camera, const int width, const int height, const float fov, const float near_val, const float far_val)
{
	float s = 1.f / tanf(fov * .5f);
	float a = 1.f / (far_val - near_val);
	TGLMat projection = {
		{ s * height / width, 0.f, 0.f, 0.f },
		{ 0.f, s, 0.f, 0.f },
		{ 0.f, 0.f, -(far_val + near_val) * a, 2.f * far_val * near_val * a },
		{ 0.f, 0.f, 1.f, 0.f },
	};
	memcpy(camera, projection, sizeof(TGLMat));
}

void tgl3d_transform_rotate(TGLTransform *const transform, const float x, const float y, const float z)
{
	const TGLMat rotate = TGL_ROTATION_MATRIX(x, y, z);
	memcpy(transform->rotate, rotate, sizeof(TGLMat));
}

void tgl3d_transform_scale(TGLTransform *const transform, const float x, const float y, const float z)
{
	const TGLMat scale = TGL_SCALE_MATRIX(x, y, z);
	memcpy(transform->scale, scale, sizeof(TGLMat));
}

void tgl3d_transform_translate(TGLTransform *const transform, const float x, const float y, const float z)
{
	const TGLMat translate = TGL_TRANSLATE_MATRIX(x, y, z);
	memcpy(transform->translate, translate, sizeof(TGLMat));
}

void tgl3d_transform_update(TGLTransform *const transform)
{
	TGLMat temp;
	tgl_mulmat(transform->translate, transform->scale, temp);
	tgl_mulmat(temp, transform->rotate, transform->result);
}

void tgl3d_transform_apply(TGLTransform *const transform, TGLVec3 in[3], TGLVec3 out[3])
{
	tgl_mulmatvec3(transform->result, in[0], out[0]);
	tgl_mulmatvec3(transform->result, in[1], out[1]);
	tgl_mulmatvec3(transform->result, in[2], out[2]);
}

float itgl_line_intersect_plane(const TGLVec3 normal, const TGLVec3 start, const TGLVec3 end, TGLVec3 point)
{
	TGLVec3 line_vec;
	tgl_sub3v(start, end, line_vec);
	const float distance = -(tgl_dot3(normal, start) + 1.f) / (tgl_dot3(normal, line_vec));
	tgl_mul3s(line_vec, distance, point);
	tgl_add3v(point, start, point);
	return distance;
}

unsigned itgl_clip_triangle_plane(const TGLVec3 normal, const TGLVec4 in[3], TGLVec4 out[2][3])
{
	/*const TGLVec3 di = {
		itgl_distance_point_plane(normal, in[0]),
		itgl_distance_point_plane(normal, in[1]),
		itgl_distance_point_plane(normal, in[2]),
	};

	unsigned n_inside = 0, n_outside = 0;
	unsigned inside[3], outside[3];
	unsigned i;
	for (i = 0; i < 3; i++) {
		if (di[i] >= 0.f)
			inside[n_inside++] = i;
		else
			outside[n_outside++] = i;
	}

	float d;

	switch (n_inside) {
	case 0:
		return 0;
	case 3:
		memcpy(&out[0], in, sizeof(TGLTriangle));
		return 1;
	case 1:
		memcpy(out[0].vertices[0], in->vertices[inside[0]], sizeof(TGLVec3));
		out[0].intensity[0] = in->intensity[inside[0]];
#pragma GCC unroll 2
		for (unsigned i = 0; i < 2; i++) {
			d = itgl_line_intersect_plane(normal, out[0].vertices[0], in->vertices[outside[i]], out[0].vertices[i + 1]);
			out[0].intensity[i + 1] = out[0].intensity[0] * (1.f - d) + in->intensity[outside[i]] * (d);
		}
		return 1;
	case 2:
		memcpy(out[0].vertices[0], in->vertices[inside[0]], sizeof(TGLVec3));
		memcpy(out[0].vertices[1], in->vertices[inside[1]], sizeof(TGLVec3));
		d = itgl_line_intersect_plane(normal, out[0].vertices[0], in->vertices[outside[0]], out[0].vertices[2]);
		out[0].intensity[0] = in->intensity[inside[0]];
		out[0].intensity[1] = in->intensity[inside[1]];
		out[0].intensity[2] = out[0].intensity[0] * (1.f - d) + in->intensity[outside[0]] * (d);

		memcpy(out[1].vertices[0], in->vertices[inside[1]], sizeof(TGLVec3));
		memcpy(out[1].vertices[1], out->vertices[2], sizeof(TGLVec3));
		d = itgl_line_intersect_plane(normal, out[1].vertices[0], in->vertices[outside[0]], out[1].vertices[2]);
		out[1].intensity[0] = in->intensity[inside[1]];
		out[1].intensity[1] = out[0].intensity[2];
		out[1].intensity[2] = out[1].intensity[0] * (1.f - d) + in->intensity[outside[0]] * (d);

		return 2;
	}
	TGL_UNREACHABLE();
	return 0;*/
	return 0;
}

void tgl3d_shader(TGL *const tgl, const TGLTriangle *in, const bool fill, TGLVertexShader *vert_shader, void *const vert_data, TGLFragmentShader *frag_shader, void *const frag_data)
{
	/*TGLTriangle out = *in;

	TGLVec4 verts[3];
	unsigned i;
	for (i = 0; i < 3; i++)
		vert_shader(in->vertices[i], verts[i], vert_data);

	if (tgl->settings & TGL_CULL_FACE) {
		TGLVec3 ab, ac, cp;
		tgl_sub3v(verts[1], verts[0], ab);
		tgl_sub3v(verts[2], verts[0], ac);
		tgl_cross(ab, ac, cp);
		if (XOR(tgl->settings & TGL_CULL_BIT, signbit(cp[2])))
			return;
	}

	TGLVec3 v[3];
	for (i = 0; i < 3; i++) {
		v[i][0] = verts[i][0] / verts[i][3];
		v[i][1] = verts[i][1] / verts[i][3];
		v[i][2] = verts[i][2] / verts[i][3];
	}

	float half_width = tgl->width * .5f;
	float half_height = tgl->height * .5f;*/

	/* FRAGMENT SHADER */
	/*if (fill)
		tgl_triangle_fill(tgl,
			MAP_COORD(half_width, trig->vertices[0][0]),
			MAP_COORD(half_height, trig->vertices[0][1]),
			trig->vertices[0][2],
			trig->intensity[0],
			MAP_COORD(half_width, trig->vertices[1][0]),
			MAP_COORD(half_height, trig->vertices[1][1]),
			trig->vertices[1][2],
			trig->intensity[1],
			MAP_COORD(half_width, trig->vertices[2][0]),
			MAP_COORD(half_height, trig->vertices[2][1]),
			trig->vertices[2][2],
			trig->intensity[2],
			color);
	else
		tgl_triangle(tgl,
			MAP_COORD(half_width, trig->vertices[0][0]),
			MAP_COORD(half_height, trig->vertices[0][1]),
			trig->vertices[0][2],
			trig->intensity[0],
			MAP_COORD(half_width, trig->vertices[1][0]),
			MAP_COORD(half_height, trig->vertices[1][1]),
			trig->vertices[1][2],
			trig->intensity[1],
			MAP_COORD(half_width, trig->vertices[2][0]),
			MAP_COORD(half_height, trig->vertices[2][1]),
			trig->vertices[2][2],
			trig->intensity[2],
			color);
			}*/
}

void tgl3d_cull_face(TGL *tgl, const uint8_t settings)
{
	tgl->settings = (tgl->settings & ~TGL_CULL_BIT) | (XOR(settings & TGL_CULL_FACE_BIT, settings & TGL_WINDING_BIT) ? TGL_CULL_BIT : 0);
}

#endif /* TERMGL3D */

#ifdef TERMGLUTIL

#ifdef __unix__
#include <sys/ioctl.h>
#include <termios.h>
#endif

TGL_SSIZE_T tglutil_read(char *const buf, const size_t count)
{
#ifdef __unix__
	struct termios oldt, newt;

	/* Disable canonical mode */
	CALL(tcgetattr(STDIN_FILENO, &oldt), -2);
	newt = oldt;
	newt.c_lflag &= ~(ICANON);
	newt.c_cc[VMIN] = 0;
	newt.c_cc[VTIME] = 0;
	CALL(tcsetattr(STDIN_FILENO, TCSANOW, &newt), -3);

	const TGL_SSIZE_T retval = read(2, buf, count);

	CALL(tcsetattr(STDIN_FILENO, TCSANOW, &oldt), -3);

	/* Flush input buffer to prevent read of previous unread input */
	CALL(tcflush(STDIN_FILENO, TCIFLUSH), -4);
#else /* defined(TGL_OS_WINDOWS) */
	const HANDLE hInputHandle = GetStdHandle(STD_INPUT_HANDLE);
	WINDOWS_CALL(hInputHandle == INVALID_HANDLE_VALUE, -1);

	/* Disable canonical mode */
	DWORD old_mode, new_mode;
	WINDOWS_CALL(!GetConsoleMode(hInputHandle, &old_mode), -1);
	new_mode = old_mode;
	new_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
	WINDOWS_CALL(!SetConsoleMode(hInputHandle, new_mode), -1);

	DWORD event_cnt;
	WINDOWS_CALL(!GetNumberOfConsoleInputEvents(hInputHandle, &event_cnt), -1);

	/* ReadConsole is blocking so must manually process events */
	size_t retval = 0;
	if (event_cnt) {
		INPUT_RECORD input_records[32];
		WINDOWS_CALL(!ReadConsoleInput(hInputHandle, input_records, 32, &event_cnt), -1);

		DWORD i;
		for (i = 0; i < event_cnt; i++) {
			if (input_records[i].Event.KeyEvent.bKeyDown && input_records[i].EventType == KEY_EVENT) {
				buf[retval++] = input_records[i].Event.KeyEvent.uChar.AsciiChar;
				if (retval == count)
					break;
			}
		}
	}

	WINDOWS_CALL(!SetConsoleMode(hInputHandle, old_mode), -1);
#endif
	return retval;
}

int tglutil_get_console_size(unsigned *const col, unsigned *const row, const bool screen_buffer)
{
#ifdef __unix__
	(void)screen_buffer;
	struct winsize w;
	const int retval = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	*col = w.ws_col;
	*row = w.ws_row;
	return retval;
#else /* defined(TGL_OS_WINDOWS) */
	const HANDLE hOutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	WINDOWS_CALL(hOutputHandle == INVALID_HANDLE_VALUE, -1);

	CONSOLE_SCREEN_BUFFER_INFO csbi;
	WINDOWS_CALL(!GetConsoleScreenBufferInfo(hOutputHandle, &csbi), -1);
	if (screen_buffer) {
		*col = csbi.dwSize.X;
		*row = csbi.dwSize.Y;
	} else {
		*col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		*row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	}
	return 0;
#endif
}

int tglutil_set_console_size(const unsigned col, const unsigned row)
{
#ifdef __unix__
	const struct winsize w = (struct winsize){
		.ws_row = row,
		.ws_col = col,
	};
	return ioctl(STDOUT_FILENO, TIOCSWINSZ, &w);
#else /* defined(TGL_OS_WINDOWS) */
	const HANDLE hOutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	WINDOWS_CALL(hOutputHandle == INVALID_HANDLE_VALUE, -1);

	const COORD size = (COORD){
		.Y = row,
		.X = col,
	};
	WINDOWS_CALL(!SetConsoleScreenBufferSize(hOutputHandle, size), -1);
	return 0;
#endif
}

#endif /* TERMGLUTIL */
