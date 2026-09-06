#define _GNU_SOURCE

#include "server.h"

#include <drm_fourcc.h>
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>

struct kono_pixel_buffer {
	struct wlr_buffer base;
	uint32_t *pixels;
	size_t stride;
};

struct jpeg_error_jmp {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

static void pixel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct kono_pixel_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	free(buffer->pixels);
	free(buffer);
}

static bool pixel_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	struct kono_pixel_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = buffer->pixels;
	*format = DRM_FORMAT_XRGB8888;
	*stride = buffer->stride;
	return true;
}

static void pixel_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
	.destroy = pixel_buffer_destroy,
	.begin_data_ptr_access = pixel_buffer_begin_data_ptr_access,
	.end_data_ptr_access = pixel_buffer_end_data_ptr_access,
};

static struct wlr_buffer *buffer_from_rgba(uint8_t *rgba, int width, int height, int src_stride) {
	struct kono_pixel_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		return NULL;
	}
	buffer->stride = (size_t)width * 4;
	buffer->pixels = calloc((size_t)height, buffer->stride);
	if (!buffer->pixels) {
		free(buffer);
		return NULL;
	}

	for (int y = 0; y < height; y++) {
		const uint8_t *srow = rgba + y * src_stride;
		uint32_t *drow = buffer->pixels + y * width;
		for (int x = 0; x < width; x++) {
			const uint8_t *px = srow + x * 4;
			uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
			uint8_t pr = (uint8_t)((r * a) / 255);
			uint8_t pg = (uint8_t)((g * a) / 255);
			uint8_t pb = (uint8_t)((b * a) / 255);
			drow[x] = (uint32_t)pb | ((uint32_t)pg << 8) | ((uint32_t)pr << 16) | 0xff000000u;
		}
	}

	wlr_buffer_init(&buffer->base, &pixel_buffer_impl, width, height);
	return &buffer->base;
}

static struct wlr_buffer *load_png(const char *path) {
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		wlr_log(WLR_ERROR, "Cannot open wallpaper %s", path);
		return NULL;
	}

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	if (!png || !info || setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(png ? &png : NULL, info ? &info : NULL, NULL);
		fclose(fp);
		wlr_log(WLR_ERROR, "Failed to decode PNG %s", path);
		return NULL;
	}

	png_init_io(png, fp);
	png_read_info(png, info);

	int width = (int)png_get_image_width(png, info);
	int height = (int)png_get_image_height(png, info);
	png_byte color_type = png_get_color_type(png, info);
	png_byte bit_depth = png_get_bit_depth(png, info);

	if (bit_depth == 16) {
		png_set_strip_16(png);
	}
	if (color_type == PNG_COLOR_TYPE_PALETTE) {
		png_set_palette_to_rgb(png);
	}
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
		png_set_expand_gray_1_2_4_to_8(png);
	}
	if (png_get_valid(png, info, PNG_INFO_tRNS)) {
		png_set_tRNS_to_alpha(png);
	}
	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png);
	}
	if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
		png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	}
	png_read_update_info(png, info);

	png_uint_32 rowbytes = png_get_rowbytes(png, info);
	uint8_t *rgba = malloc((size_t)height * rowbytes);
	png_bytep *rows = malloc((size_t)height * sizeof(*rows));
	if (!rgba || !rows) {
		free(rgba);
		free(rows);
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return NULL;
	}
	for (int y = 0; y < height; y++) {
		rows[y] = rgba + y * rowbytes;
	}
	png_read_image(png, rows);
	png_read_end(png, NULL);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);
	free(rows);

	struct wlr_buffer *buffer = buffer_from_rgba(rgba, width, height, (int)rowbytes);
	free(rgba);
	return buffer;
}

static void jpeg_error_exit(j_common_ptr cinfo) {
	struct jpeg_error_jmp *err = (struct jpeg_error_jmp *)cinfo->err;
	longjmp(err->setjmp_buffer, 1);
}

static struct wlr_buffer *load_jpeg(const char *path) {
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		wlr_log(WLR_ERROR, "Cannot open wallpaper %s", path);
		return NULL;
	}

	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_jmp jerr;
	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpeg_error_exit;
	if (setjmp(jerr.setjmp_buffer)) {
		jpeg_destroy_decompress(&cinfo);
		fclose(fp);
		wlr_log(WLR_ERROR, "Failed to decode JPEG %s", path);
		return NULL;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, fp);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	int width = (int)cinfo.output_width;
	int height = (int)cinfo.output_height;
	int src_stride = width * (int)cinfo.output_components;
	uint8_t *rgb = malloc((size_t)height * (size_t)src_stride);
	uint8_t *rgba = malloc((size_t)height * (size_t)width * 4);
	if (!rgb || !rgba) {
		free(rgb);
		free(rgba);
		jpeg_destroy_decompress(&cinfo);
		fclose(fp);
		return NULL;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		uint8_t *row = rgb + cinfo.output_scanline * src_stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(fp);

	for (int y = 0; y < height; y++) {
		const uint8_t *srow = rgb + y * src_stride;
		uint8_t *drow = rgba + y * width * 4;
		for (int x = 0; x < width; x++) {
			drow[x * 4 + 0] = srow[x * 3 + 0];
			drow[x * 4 + 1] = srow[x * 3 + 1];
			drow[x * 4 + 2] = srow[x * 3 + 2];
			drow[x * 4 + 3] = 255;
		}
	}
	free(rgb);

	struct wlr_buffer *buffer = buffer_from_rgba(rgba, width, height, width * 4);
	free(rgba);
	return buffer;
}

static const char *file_ext(const char *path) {
	const char *dot = strrchr(path, '.');
	return dot ? dot + 1 : "";
}

static struct wlr_buffer *load_image_buffer(const char *path) {
	const char *ext = file_ext(path);
	if (strcasecmp(ext, "png") == 0) {
		return load_png(path);
	}
	if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
		return load_jpeg(path);
	}

	FILE *fp = fopen(path, "rb");
	if (!fp) {
		wlr_log(WLR_ERROR, "Cannot open wallpaper %s", path);
		return NULL;
	}
	unsigned char sig[8] = {0};
	size_t n = fread(sig, 1, sizeof(sig), fp);
	fclose(fp);
	if (n >= 8 && png_sig_cmp(sig, 0, 8) == 0) {
		return load_png(path);
	}
	if (n >= 2 && sig[0] == 0xff && sig[1] == 0xd8) {
		return load_jpeg(path);
	}
	wlr_log(WLR_ERROR, "Unsupported wallpaper format: %s", path);
	return NULL;
}

static bool wallpaper_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static void apply_buffer_to_output(struct kono_output *output, struct wlr_buffer *buffer) {
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	struct wlr_box layout_box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &layout_box);

	if (!output->wallpaper_image) {
		output->wallpaper_image = wlr_scene_buffer_create(
				output->server->layers[KONO_LAYER_BACKGROUND], buffer);
	} else {
		wlr_scene_buffer_set_buffer(output->wallpaper_image, buffer);
	}
	if (!output->wallpaper_image) {
		return;
	}
	output->wallpaper_image->point_accepts_input = wallpaper_accepts_input;
	wlr_scene_node_set_position(&output->wallpaper_image->node, layout_box.x, layout_box.y);
	wlr_scene_buffer_set_dest_size(output->wallpaper_image, width, height);
	wlr_scene_node_set_enabled(&output->wallpaper_image->node, true);
	wlr_scene_node_raise_to_top(&output->wallpaper_image->node);
}

void kono_output_apply_wallpaper(struct kono_output *output) {
	if (!output || !output->server->wallpaper_buffer) {
		if (output && output->wallpaper) {
			wlr_scene_node_set_enabled(&output->wallpaper->node, true);
		}
		if (output && output->wallpaper_image) {
			wlr_scene_node_set_enabled(&output->wallpaper_image->node, false);
		}
		return;
	}
	apply_buffer_to_output(output, output->server->wallpaper_buffer);
}

bool kono_set_wallpaper(struct kono_server *server, const char *path) {
	if (!server || !path || !path[0]) {
		return false;
	}

	wlr_log(WLR_INFO, "Loading wallpaper %s", path);
	struct wlr_buffer *buffer = load_image_buffer(path);
	if (!buffer) {
		wlr_log(WLR_ERROR, "Could not decode wallpaper %s", path);
		return false;
	}

	if (server->wallpaper_buffer) {
		wlr_buffer_drop(server->wallpaper_buffer);
		server->wallpaper_buffer = NULL;
	}
	server->wallpaper_buffer = buffer;
	snprintf(server->wallpaper_path, sizeof(server->wallpaper_path), "%s", path);

	struct kono_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		kono_output_apply_wallpaper(output);
		wlr_output_schedule_frame(output->wlr_output);
	}

	char state[512];
	kono_config_dir(state, sizeof(state));
	char file[640];
	snprintf(file, sizeof(file), "%s/current-wallpaper", state);
	FILE *f = fopen(file, "w");
	if (f) {
		fprintf(f, "%s\n", path);
		fclose(f);
	}

	wlr_log(WLR_INFO, "Wallpaper set: %s", path);
	return true;
}

void kono_reload_wallpaper(struct kono_server *server) {
	char path[512] = {0};
	char dir[512];
	kono_config_dir(dir, sizeof(dir));
	char state[640];
	snprintf(state, sizeof(state), "%s/current-wallpaper", dir);

	FILE *f = fopen(state, "r");
	if (f) {
		if (fgets(path, sizeof(path), f)) {
			path[strcspn(path, "\r\n")] = '\0';
		}
		fclose(f);
	}
	if (path[0]) {
		kono_set_wallpaper(server, path);
	}
}
