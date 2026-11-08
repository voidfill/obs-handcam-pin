/*
 * handcam-pin — OBS Studio video filter that perspective-corrects an angled
 * webcam ("handcam"). Four corners (normalized 0..1 source coordinates) define
 * a quad in the source image; the filter warps that quad to fill the output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <sys/stat.h> /* before util/platform.h (declares os_stat) */

#include <obs-module.h>
#include <util/platform.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
extern char **environ;
#endif

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0-dev"
#endif

#define FILTER_ID "handcam_pin"
#define LOG_PREFIX "[handcam-pin] "

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("handcam-pin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Perspective-correct an angled handcam by pinning four corners.";
}

/* Corner order everywhere: TL, TR, BR, BL; each as (x, y). */
enum { TL_X, TL_Y, TR_X, TR_Y, BR_X, BR_Y, BL_X, BL_Y, NUM_COORDS };

static const char *const corner_keys[NUM_COORDS] = {"tl_x", "tl_y", "tr_x", "tr_y",
						     "br_x", "br_y", "bl_x", "bl_y"};

static const char *const corner_labels[NUM_COORDS] = {"TopLeftX",     "TopLeftY",     "TopRightX",
						       "TopRightY",    "BottomRightX", "BottomRightY",
						       "BottomLeftX",  "BottomLeftY"};

static const double default_corners[NUM_COORDS] = {0.1, 0.1, 0.9, 0.1, 0.9, 0.9, 0.1, 0.9};

struct handcam_pin {
	obs_source_t *source;

	gs_effect_t *effect;
	gs_eparam_t *p_row0;
	gs_eparam_t *p_row1;
	gs_eparam_t *p_row2;

	double corners[NUM_COORDS]; /* last valid corners */
	float h[9];                 /* row-major 3x3, unit square -> source uv */
	uint32_t out_width;         /* 0 = source size */
	uint32_t out_height;
};

/* ------------------------------------------------------------------------ */
/* Math                                                                     */

/* Strictly convex, non-degenerate quad (either winding; mirroring flips it). */
static bool quad_is_convex(const double c[NUM_COORDS])
{
	int sign = 0;
	double area2 = 0.0;

	for (int i = 0; i < 4; i++) {
		const double *p0 = &c[2 * i];
		const double *p1 = &c[2 * ((i + 1) % 4)];
		const double *p2 = &c[2 * ((i + 2) % 4)];
		double ex1 = p1[0] - p0[0], ey1 = p1[1] - p0[1];
		double ex2 = p2[0] - p1[0], ey2 = p2[1] - p1[1];
		double cross = ex1 * ey2 - ey1 * ex2;

		if (fabs(cross) < 1e-9)
			return false;
		int s = cross > 0.0 ? 1 : -1;
		if (sign == 0)
			sign = s;
		else if (s != sign)
			return false;

		area2 += p0[0] * p1[1] - p1[0] * p0[1];
	}
	return fabs(area2) > 1e-6;
}

/*
 * Heckbert square-to-quad projective mapping. Maps (0,0),(1,0),(1,1),(0,1)
 * to TL,TR,BR,BL:  x = (a u + b v + c) / (g u + h v + 1), etc.
 * Output row-major: [a b c; d e f; g h 1].
 */
static bool compute_homography(const double q[NUM_COORDS], float out[9])
{
	for (int i = 0; i < NUM_COORDS; i++) {
		if (!isfinite(q[i]))
			return false;
	}
	if (!quad_is_convex(q))
		return false;

	const double x0 = q[TL_X], y0 = q[TL_Y];
	const double x1 = q[TR_X], y1 = q[TR_Y];
	const double x2 = q[BR_X], y2 = q[BR_Y];
	const double x3 = q[BL_X], y3 = q[BL_Y];

	const double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
	const double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;

	double a, b, c, d, e, f, g, h;

	if (fabs(dx3) < 1e-12 && fabs(dy3) < 1e-12) {
		/* Parallelogram: affine. */
		a = x1 - x0;
		b = x2 - x1;
		c = x0;
		d = y1 - y0;
		e = y2 - y1;
		f = y0;
		g = 0.0;
		h = 0.0;
	} else {
		const double det = dx1 * dy2 - dx2 * dy1;
		if (fabs(det) < 1e-12)
			return false;
		g = (dx3 * dy2 - dx2 * dy3) / det;
		h = (dx1 * dy3 - dx3 * dy1) / det;
		a = x1 - x0 + g * x1;
		b = x3 - x0 + h * x3;
		c = x0;
		d = y1 - y0 + g * y1;
		e = y3 - y0 + h * y3;
		f = y0;
	}

	const double m[9] = {a, b, c, d, e, f, g, h, 1.0};
	for (int i = 0; i < 9; i++) {
		if (!isfinite(m[i]))
			return false;
		out[i] = (float)m[i];
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/* Source callbacks                                                         */

static const char *hp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("HandcamPin");
}

static void hp_defaults(obs_data_t *settings)
{
	for (int i = 0; i < NUM_COORDS; i++)
		obs_data_set_default_double(settings, corner_keys[i], default_corners[i]);
	obs_data_set_default_int(settings, "out_width", 0);
	obs_data_set_default_int(settings, "out_height", 0);
}

static void hp_update(void *data, obs_data_t *settings)
{
	struct handcam_pin *f = data;
	double c[NUM_COORDS];
	float h[9];

	for (int i = 0; i < NUM_COORDS; i++)
		c[i] = obs_data_get_double(settings, corner_keys[i]);

	if (compute_homography(c, h)) {
		memcpy(f->corners, c, sizeof(c));
		memcpy(f->h, h, sizeof(h));
	} else {
		blog(LOG_DEBUG, LOG_PREFIX "'%s': ignoring non-convex/degenerate quad, keeping previous warp",
		     obs_source_get_name(f->source));
	}

	long long ow = obs_data_get_int(settings, "out_width");
	long long oh = obs_data_get_int(settings, "out_height");
	f->out_width = ow > 0 ? (uint32_t)ow : 0;
	f->out_height = oh > 0 ? (uint32_t)oh : 0;
}

static void *hp_create(obs_data_t *settings, obs_source_t *source)
{
	struct handcam_pin *f = bzalloc(sizeof(*f));
	f->source = source;

	/* Always start from a valid matrix so a bad saved quad can't break us. */
	memcpy(f->corners, default_corners, sizeof(default_corners));
	compute_homography(default_corners, f->h);

	char *path = obs_module_file("handcam-pin.effect");
	char *errors = NULL;

	obs_enter_graphics();
	if (path)
		f->effect = gs_effect_create_from_file(path, &errors);
	if (f->effect) {
		f->p_row0 = gs_effect_get_param_by_name(f->effect, "h_row0");
		f->p_row1 = gs_effect_get_param_by_name(f->effect, "h_row1");
		f->p_row2 = gs_effect_get_param_by_name(f->effect, "h_row2");
	}
	obs_leave_graphics();

	if (!f->effect)
		blog(LOG_ERROR, LOG_PREFIX "failed to load effect '%s': %s", path ? path : "(not found)",
		     errors ? errors : "(no compiler output)");

	bfree(errors);
	bfree(path);

	hp_update(f, settings);
	return f;
}

static void hp_destroy(void *data)
{
	struct handcam_pin *f = data;

	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
	}
	bfree(f);
}

static uint32_t hp_get_width(void *data)
{
	struct handcam_pin *f = data;
	if (f->out_width)
		return f->out_width;
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_width(target) : 0;
}

static uint32_t hp_get_height(void *data)
{
	struct handcam_pin *f = data;
	if (f->out_height)
		return f->out_height;
	obs_source_t *target = obs_filter_get_target(f->source);
	return target ? obs_source_get_base_height(target) : 0;
}

static void hp_render(void *data, gs_effect_t *unused)
{
	struct handcam_pin *f = data;
	UNUSED_PARAMETER(unused);

	obs_source_t *target = obs_filter_get_target(f->source);
	if (!f->effect || !target || !obs_source_get_base_width(target) || !obs_source_get_base_height(target)) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	uint32_t w = hp_get_width(f);
	uint32_t h = hp_get_height(f);
	if (!w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	/* No direct rendering: the output may be a different size than the source. */
	if (!obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	float m[9];
	memcpy(m, f->h, sizeof(m));

	struct vec3 r0, r1, r2;
	vec3_set(&r0, m[0], m[1], m[2]);
	vec3_set(&r1, m[3], m[4], m[5]);
	vec3_set(&r2, m[6], m[7], m[8]);
	gs_effect_set_vec3(f->p_row0, &r0);
	gs_effect_set_vec3(f->p_row1, &r1);
	gs_effect_set_vec3(f->p_row2, &r2);

	obs_source_process_filter_end(f->source, f->effect, w, h);
}

/* ------------------------------------------------------------------------ */
/* Properties                                                               */

static bool set_corners(struct handcam_pin *f, const double c[NUM_COORDS])
{
	obs_data_t *s = obs_data_create();
	for (int i = 0; i < NUM_COORDS; i++)
		obs_data_set_double(s, corner_keys[i], c[i]);
	obs_source_update(f->source, s);
	obs_data_release(s);
	return true; /* refresh the properties view so sliders follow */
}

static bool on_rotate(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct handcam_pin *f = data;
	const double *o = f->corners;
	/* TL <- BL <- BR <- TR <- TL */
	const double c[NUM_COORDS] = {o[BL_X], o[BL_Y], o[TL_X], o[TL_Y], o[TR_X], o[TR_Y], o[BR_X], o[BR_Y]};
	return set_corners(f, c);
}

static bool on_mirror(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct handcam_pin *f = data;
	const double *o = f->corners;
	/* swap TL<->TR, BL<->BR */
	const double c[NUM_COORDS] = {o[TR_X], o[TR_Y], o[TL_X], o[TL_Y], o[BL_X], o[BL_Y], o[BR_X], o[BR_Y]};
	return set_corners(f, c);
}

static bool on_reset(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	return set_corners(data, default_corners);
}

/* Absolute filesystem path of the bundled editor (bfree the result). */
static char *editor_path(void)
{
	char *rel = obs_module_file("editor.html");
	if (!rel)
		return NULL;
	char *abs = os_get_abs_path_ptr(rel);
	bfree(rel);
	if (abs) {
		for (char *s = abs; *s; s++) {
			if (*s == '\\')
				*s = '/';
		}
	}
	return abs;
}

/* file:// URL for a path, percent-encoding everything but unreserved chars. */
static char *path_to_file_url(const char *path)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t len = strlen(path);
	char *url = bmalloc(len * 3 + 16);
	char *o = url;

	memcpy(o, "file://", 7);
	o += 7;
	if (path[0] != '/')
		*o++ = '/'; /* Windows drive path: file:///C:/... */

	for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
		unsigned char ch = *s;
		bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
			    ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':';
		if (keep) {
			*o++ = (char)ch;
		} else {
			*o++ = '%';
			*o++ = hex[ch >> 4];
			*o++ = hex[ch & 15];
		}
	}
	*o = '\0';
	return url;
}

static void open_with_os(const char *path)
{
#ifdef _WIN32
	wchar_t *wpath = NULL;
	if (os_utf8_to_wcs_ptr(path, 0, &wpath) && wpath) {
		HINSTANCE r = ShellExecuteW(NULL, L"open", wpath, NULL, NULL, SW_SHOWNORMAL);
		if ((INT_PTR)r <= 32)
			blog(LOG_WARNING, LOG_PREFIX "ShellExecute failed (%d) for '%s'", (int)(INT_PTR)r, path);
	}
	bfree(wpath);
#else
#ifdef __APPLE__
	const char *script = "open \"$1\" >/dev/null 2>&1 &";
#else
	const char *script = "xdg-open \"$1\" >/dev/null 2>&1 &";
#endif
	/* sh backgrounds the opener and exits at once, so the wait is immediate. */
	char *argv[] = {"sh", "-c", (char *)script, "sh", (char *)path, NULL};
	pid_t pid;
	if (posix_spawnp(&pid, "sh", NULL, NULL, argv, environ) == 0) {
		int status;
		waitpid(pid, &status, 0);
	} else {
		blog(LOG_WARNING, LOG_PREFIX "could not launch opener for '%s'", path);
	}
#endif
}

static bool on_open_editor(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);

	char *path = editor_path();
	if (path) {
		open_with_os(path);
		bfree(path);
	} else {
		blog(LOG_WARNING, LOG_PREFIX "editor.html not found in module data");
	}
	return false;
}

static obs_properties_t *hp_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *corners = obs_properties_create();
	for (int i = 0; i < NUM_COORDS; i++)
		obs_properties_add_float_slider(corners, corner_keys[i], obs_module_text(corner_labels[i]), 0.0, 1.0,
						0.0005);
	obs_properties_add_button(corners, "rotate", obs_module_text("Rotate90"), on_rotate);
	obs_properties_add_button(corners, "mirror", obs_module_text("Mirror"), on_mirror);
	obs_properties_add_button(corners, "reset", obs_module_text("Reset"), on_reset);
	obs_properties_add_group(props, "corners", obs_module_text("Corners"), OBS_GROUP_NORMAL, corners);

	obs_properties_t *output = obs_properties_create();
	obs_property_t *ow = obs_properties_add_int(output, "out_width", obs_module_text("OutWidth"), 0, 16384, 1);
	obs_property_int_set_suffix(ow, " px");
	obs_property_t *oh = obs_properties_add_int(output, "out_height", obs_module_text("OutHeight"), 0, 16384, 1);
	obs_property_int_set_suffix(oh, " px");
	obs_properties_add_text(output, "out_hint", obs_module_text("OutHint"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "output", obs_module_text("Output"), OBS_GROUP_NORMAL, output);

	obs_properties_t *editor = obs_properties_create();
	obs_properties_add_button(editor, "open_editor", obs_module_text("OpenEditor"), on_open_editor);

	char *path = editor_path();
	if (path) {
		/* Selectable label with the file:// URL, for adding the editor as a dock. */
		char *url = path_to_file_url(path);
		const char *hint = obs_module_text("EditorUrlHint");
		size_t n = strlen(hint) + strlen(url) + 32;
		char *info = bmalloc(n);
		snprintf(info, n, "%s<br><code>%s</code>", hint, url);
		obs_properties_add_text(editor, "editor_url", info, OBS_TEXT_INFO);
		bfree(info);
		bfree(url);
		bfree(path);
	}
	obs_properties_add_group(props, "editor", obs_module_text("Editor"), OBS_GROUP_NORMAL, editor);

	return props;
}

/* ------------------------------------------------------------------------ */

static struct obs_source_info handcam_pin_filter = {
	.id = FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = hp_get_name,
	.create = hp_create,
	.destroy = hp_destroy,
	.update = hp_update,
	.get_defaults = hp_defaults,
	.get_properties = hp_properties,
	.video_render = hp_render,
	.get_width = hp_get_width,
	.get_height = hp_get_height,
};

bool obs_module_load(void)
{
	obs_register_source(&handcam_pin_filter);
	blog(LOG_INFO, LOG_PREFIX "loaded version %s", PLUGIN_VERSION);
	return true;
}
