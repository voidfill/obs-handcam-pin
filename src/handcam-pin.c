/*
 * handcam-pin — OBS Studio video filter that perspective-corrects an angled
 * webcam ("handcam"). Four corners (normalized 0..1 source coordinates) define
 * a quad in the source image; the filter warps that quad to fill the output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <sys/stat.h> /* before util/platform.h (declares os_stat) */

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <stdatomic.h> /* not util/threading.h: it needs MSVC intrinsics on Windows */

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
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron()) /* bundles can't use environ directly */
#else
extern char **environ;
#endif
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
	double aspect;              /* output width/height; 0 = same as the camera */

	/* Copy of our input for the editor's raw view (see "Raw frame for the
	 * editor" below). Only touched while holding the graphics context. */
	gs_texrender_t *raw_tex;
	gs_stagesurf_t *raw_stage;
	uint32_t raw_max;       /* wanted longest side in pixels, 0 = full size */
	bool raw_want;          /* capture during the next render */
	bool raw_done;          /* raw_stage holds a fresh capture */
	uint32_t raw_w, raw_h;  /* size of that capture */
	uint32_t raw_sw, raw_sh; /* size of the input it was scaled from */
	atomic_bool raw_signal; /* set by hp_render after a capture */
	atomic_bool raw_busy;   /* a vendor request is using the fields above */
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

/* file:// URL of the editor, set in obs_module_post_load. */
static char *editor_url;

static void hp_defaults(obs_data_t *settings)
{
	/* Shown in a text field so it can be copied (OBS info labels aren't selectable). */
	obs_data_set_default_string(settings, "editor_url", editor_url ? editor_url : "");

	for (int i = 0; i < NUM_COORDS; i++)
		obs_data_set_default_double(settings, corner_keys[i], default_corners[i]);
	obs_data_set_default_string(settings, "shape", "pad");
	obs_data_set_default_double(settings, "pad_width", 45.0);
	obs_data_set_default_double(settings, "pad_height", 40.0);
}

/* 0.0.x stored an explicit output size in pixels; keep its shape as a ratio. */
static void migrate_settings(obs_data_t *s)
{
	long long ow = obs_data_get_int(s, "out_width");
	long long oh = obs_data_get_int(s, "out_height");
	if (!obs_data_has_user_value(s, "shape") && ow > 0 && oh > 0) {
		obs_data_set_string(s, "shape", "pad");
		obs_data_set_double(s, "pad_width", round(400.0 * (double)ow / (double)oh) / 10.0);
		obs_data_set_double(s, "pad_height", 40.0);
	} else if (!obs_data_has_user_value(s, "shape") && (obs_data_has_user_value(s, "out_width") ||
							    obs_data_has_user_value(s, "out_height"))) {
		obs_data_set_string(s, "shape", "source"); /* old 0 = source size */
	}
	obs_data_erase(s, "out_width");
	obs_data_erase(s, "out_height");
}

/* Output = largest size with the wanted ratio that fits inside the camera frame,
 * so we never invent pixels; it only changes when the ratio (or camera) does. */
static uint32_t even_px(double v)
{
	uint32_t p = (uint32_t)(v / 2.0) * 2; /* round down: never larger than the camera */
	return p < 2 ? 2 : p;
}

static void output_size(struct handcam_pin *f, uint32_t *ow, uint32_t *oh)
{
	obs_source_t *target = obs_filter_get_target(f->source);
	uint32_t sw = target ? obs_source_get_base_width(target) : 0;
	uint32_t sh = target ? obs_source_get_base_height(target) : 0;
	double r = f->aspect;

	*ow = sw;
	*oh = sh;
	if (!sw || !sh || r <= 0.0)
		return;
	if (r >= (double)sw / (double)sh) {
		*ow = even_px(sw);
		*oh = even_px(sw / r);
	} else {
		*ow = even_px(sh * r);
		*oh = even_px(sh);
	}
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

	const char *shape = obs_data_get_string(settings, "shape");
	double pw = obs_data_get_double(settings, "pad_width");
	double ph = obs_data_get_double(settings, "pad_height");
	double aspect = 0.0; /* "source" */
	if (shape && strcmp(shape, "square") == 0)
		aspect = 1.0;
	else if (shape && strcmp(shape, "pad") == 0 && isfinite(pw) && isfinite(ph) && pw > 0.0 && ph > 0.0)
		aspect = fmin(20.0, fmax(0.05, pw / ph));
	f->aspect = aspect;
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

	migrate_settings(settings);
	hp_update(f, settings);
	return f;
}

static void hp_destroy(void *data)
{
	struct handcam_pin *f = data;

	obs_enter_graphics();
	if (f->effect)
		gs_effect_destroy(f->effect);
	if (f->raw_tex)
		gs_texrender_destroy(f->raw_tex);
	if (f->raw_stage)
		gs_stagesurface_destroy(f->raw_stage);
	obs_leave_graphics();
	bfree(f);
}

static uint32_t hp_get_width(void *data)
{
	uint32_t w, h;
	output_size(data, &w, &h);
	return w;
}

static uint32_t hp_get_height(void *data)
{
	uint32_t w, h;
	output_size(data, &w, &h);
	return h;
}

/*
 * Render our input (the source, plus any filters before us) scaled down into
 * raw_tex and queue a GPU->CPU copy. obs_source_skip_video_filter() draws the
 * same input libobs hands us, without our warp.
 */
static void capture_raw_input(struct handcam_pin *f)
{
	f->raw_want = false;

	obs_source_t *target = obs_filter_get_target(f->source);
	uint32_t sw = target ? obs_source_get_base_width(target) : 0;
	uint32_t sh = target ? obs_source_get_base_height(target) : 0;
	if (!sw || !sh)
		return;

	uint32_t cw = sw, ch = sh, longest = sw > sh ? sw : sh;
	if (f->raw_max && longest > f->raw_max) {
		double s = (double)f->raw_max / (double)longest;
		cw = (uint32_t)fmax(1.0, round(sw * s));
		ch = (uint32_t)fmax(1.0, round(sh * s));
	}

	if (f->raw_stage &&
	    (gs_stagesurface_get_width(f->raw_stage) != cw || gs_stagesurface_get_height(f->raw_stage) != ch)) {
		gs_stagesurface_destroy(f->raw_stage);
		f->raw_stage = NULL;
	}
	if (!f->raw_stage)
		f->raw_stage = gs_stagesurface_create(cw, ch, GS_RGBA);
	if (!f->raw_tex)
		f->raw_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!f->raw_stage || !f->raw_tex)
		return;

	gs_texrender_reset(f->raw_tex);
	if (!gs_texrender_begin(f->raw_tex, cw, ch))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_skip_video_filter(f->source); /* draws exactly our input */
	gs_blend_state_pop();
	gs_texrender_end(f->raw_tex);

	gs_stage_texture(f->raw_stage, gs_texrender_get_texture(f->raw_tex));
	f->raw_w = cw;
	f->raw_h = ch;
	f->raw_sw = sw;
	f->raw_sh = sh;
	f->raw_done = true;
}

static void hp_render(void *data, gs_effect_t *unused)
{
	struct handcam_pin *f = data;
	UNUSED_PARAMETER(unused);

	if (f->raw_want) {
		capture_raw_input(f);
		atomic_store(&f->raw_signal, true);
	}

	uint32_t w, h;
	output_size(f, &w, &h);
	if (!f->effect || !w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	/* No direct rendering: the output may be a different size than the source. */
	if (!obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	const float *m = f->h;
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

/* Absolute path with forward slashes (takes ownership of rel, bfree the result). */
static char *abs_slashes(char *rel)
{
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

/*
 * The editor is a static HTML file, so it can't ask OBS for the websocket
 * password. Instead we write a copy of it into our plugin_config folder with
 * obs-websocket's current port/password filled in. "Open editor" and the
 * dock URL point at that copy. It's regenerated on every OBS start and whenever "Open editor" is
 * pressed, so password changes are picked up. The password is already stored
 * in plain text in obs-websocket's config.json in the same folder tree.
 */
#define CONFIG_MARKER "/*HANDCAM_PIN_CONFIG*/null"

static char *bundled_editor_path(void)
{
	return abs_slashes(obs_module_file("editor.html"));
}

/* Our plugin_config folder, created if needed. On POSIX os_get_abs_path_ptr()
 * uses realpath(), which fails for paths that don't exist yet, so only the
 * (now existing) folder is made absolute and the file name appended after. */
static char *live_editor_path(void)
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return NULL;
	os_mkdirs(dir);
	char *abs = abs_slashes(dir);
	if (!abs)
		return NULL;
	struct dstr path = {0};
	dstr_copy(&path, abs);
	bfree(abs);
	if (path.len && path.array[path.len - 1] != '/')
		dstr_cat(&path, "/");
	dstr_cat(&path, "editor.html");
	return path.array;
}

/* plugin_config/obs-websocket/config.json, built from our own config folder
 * by text (a ".." path would need our folder to exist on POSIX). */
static char *websocket_config_path(void)
{
	char *dir = obs_module_config_path(""); /* .../plugin_config/handcam-pin/ */
	if (!dir)
		return NULL;
	size_t n = strlen(dir);
	while (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
		n--;
	while (n && dir[n - 1] != '/' && dir[n - 1] != '\\')
		n--;
	struct dstr path = {0};
	dstr_ncopy(&path, dir, n);
	dstr_cat(&path, "obs-websocket/config.json");
	bfree(dir);
	return path.array;
}

/* JSON for the editor, or NULL if obs-websocket has no config yet. */
static char *websocket_config_json(void)
{
	char *ws_path = websocket_config_path();
	obs_data_t *ws = ws_path ? obs_data_create_from_json_file(ws_path) : NULL;
	bfree(ws_path);
	if (!ws)
		return NULL;

	obs_data_t *out = obs_data_create();
	obs_data_set_int(out, "port", obs_data_has_user_value(ws, "server_port") ? obs_data_get_int(ws, "server_port")
										 : 4455);
	bool auth = !obs_data_has_user_value(ws, "auth_required") || obs_data_get_bool(ws, "auth_required");
	obs_data_set_string(out, "password", auth ? obs_data_get_string(ws, "server_password") : "");

	/* Escape '<' so a password can't close the <script> it's embedded in. */
	struct dstr json = {0};
	dstr_copy(&json, obs_data_get_json(out));
	dstr_replace(&json, "<", "\\u003c");
	obs_data_release(out);
	obs_data_release(ws);
	return json.array;
}

/* Write the editor copy with connection details. Returns its path, or NULL. */
static char *write_live_editor(void)
{
	char *src = bundled_editor_path();
	char *html = src ? os_quick_read_utf8_file(src) : NULL;
	bfree(src);
	if (!html)
		return NULL;

	/* Without a websocket config the marker stays (it reads as null). */
	struct dstr out = {0};
	dstr_copy(&out, html);
	bfree(html);
	char *cfg = websocket_config_json();
	if (cfg)
		dstr_replace(&out, CONFIG_MARKER, cfg);
	bfree(cfg);

	/* Written via a temp file, so a dock loading it never sees half a page. */
	char *dst = live_editor_path();
	if (dst && !os_quick_write_utf8_file_safe(dst, out.array, out.len, false, "tmp", NULL)) {
		blog(LOG_WARNING, LOG_PREFIX "could not write '%s'", dst);
		bfree(dst);
		dst = NULL;
	}
	dstr_free(&out);
	return dst;
}

/* The editor to open: the live copy if it exists, else the bundled file. */
static char *editor_path(void)
{
	char *live = live_editor_path();
	if (live && os_file_exists(live))
		return live;
	bfree(live);
	return bundled_editor_path();
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
	if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) == 0) {
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

	char *path = write_live_editor(); /* refresh connection details */
	if (!path)
		path = editor_path();
	if (path) {
		open_with_os(path);
		bfree(path);
	} else {
		blog(LOG_WARNING, LOG_PREFIX "editor.html not found in module data");
	}
	return false;
}

/* The Dock URL field is for copying only: don't keep anything typed into it.
 * (Runs on the UI thread, which owns the settings while the dialog is open.) */
static bool on_editor_url_edited(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	obs_data_unset_user_value(settings, "editor_url");
	return false;
}

/* Mousepad size fields only matter for the "pad" shape. */
static bool on_shape_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *shape = obs_data_get_string(settings, "shape");
	bool pad = shape && strcmp(shape, "pad") == 0;
	obs_property_set_visible(obs_properties_get(props, "pad_width"), pad);
	obs_property_set_visible(obs_properties_get(props, "pad_height"), pad);
	obs_property_set_visible(obs_properties_get(props, "pad_hint"), pad);
	return true;
}

static obs_properties_t *hp_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *editor = obs_properties_create();
	obs_properties_add_button(editor, "open_editor", obs_module_text("OpenEditor"), on_open_editor);
	obs_property_t *url = obs_properties_add_text(editor, "editor_url", obs_module_text("EditorUrl"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(url, on_editor_url_edited);
	obs_properties_add_text(editor, "editor_hint", obs_module_text("EditorHint"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "editor", obs_module_text("Editor"), OBS_GROUP_NORMAL, editor);

	obs_properties_t *corners = obs_properties_create();
	for (int i = 0; i < NUM_COORDS; i++)
		obs_properties_add_float_slider(corners, corner_keys[i], obs_module_text(corner_labels[i]), 0.0, 1.0,
						0.0005);
	obs_properties_add_button(corners, "rotate", obs_module_text("Rotate90"), on_rotate);
	obs_properties_add_button(corners, "mirror", obs_module_text("Mirror"), on_mirror);
	obs_properties_add_button(corners, "reset", obs_module_text("Reset"), on_reset);
	obs_properties_add_group(props, "corners", obs_module_text("Corners"), OBS_GROUP_NORMAL, corners);

	obs_properties_t *output = obs_properties_create();
	obs_property_t *shape = obs_properties_add_list(output, "shape", obs_module_text("Shape"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(shape, obs_module_text("ShapePad"), "pad");
	obs_property_list_add_string(shape, obs_module_text("ShapeSquare"), "square");
	obs_property_list_add_string(shape, obs_module_text("ShapeSource"), "source");
	obs_property_set_modified_callback(shape, on_shape_changed);
	obs_property_t *pw = obs_properties_add_float(output, "pad_width", obs_module_text("PadWidth"), 1.0, 1000.0, 0.1);
	obs_property_float_set_suffix(pw, " cm");
	obs_property_t *ph = obs_properties_add_float(output, "pad_height", obs_module_text("PadHeight"), 1.0, 1000.0, 0.1);
	obs_property_float_set_suffix(ph, " cm");
	obs_properties_add_text(output, "pad_hint", obs_module_text("PadHint"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "output", obs_module_text("Output"), OBS_GROUP_NORMAL, output);

	return props;
}

/* ------------------------------------------------------------------------ */
/* Raw frame for the editor                                                 */
/*
 * The editor needs our *input* (camera before the warp) to place corners on.
 * obs-websocket can only screenshot a source with all its filters applied, so
 * we answer an obs-websocket vendor request instead:
 *
 *   CallVendorRequest { vendorName: "handcam-pin", requestType: "GetRawFrame",
 *                       requestData: { source, filter, max_size } }
 *   -> { image: "data:image/bmp;base64,...", width, height,
 *        source_width, source_height }   or   { error: "..." }
 *
 * The handler arms a capture and waits for OBS's next render of the source,
 * in which hp_render copies its input. (Rendering the source ourselves only
 * works when OBS isn't drawing it: filters after ours would just redraw their
 * cached texture for this frame and never reach hp_render.) If nothing draws
 * the source within a few frames, the handler renders it itself, the way
 * GetSourceScreenshot does. The copy is read back a frame later so OBS never
 * waits on the GPU for it. The filter is never switched off, so the stream
 * doesn't flicker and the editor can poll this for a live view.
 */
#define VENDOR_NAME "handcam-pin"

/* Mirrors obs-websocket's lib/obs-websocket-api.h (copied by obs-websocket). */
struct ws_request_callback {
	void (*callback)(obs_data_t *request, obs_data_t *response, void *priv);
	void *priv;
};

static void put_le16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	put_le16(p, v);
	put_le16(p + 2, v >> 16);
}

/* Tightly packed RGBA -> 24-bit top-down BMP as a data: URL (bfree it).
 * Browsers decode BMP natively, and it costs nothing to encode. */
static char *rgba_to_bmp_url(const uint8_t *rgba, uint32_t w, uint32_t h)
{
	static const char prefix[] = "data:image/bmp;base64,";
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
	const size_t size = 54 + row * h;

	uint8_t *bmp = bzalloc(size);
	bmp[0] = 'B';
	bmp[1] = 'M';
	put_le32(bmp + 2, (uint32_t)size);
	put_le32(bmp + 10, 54);                  /* pixel data offset */
	put_le32(bmp + 14, 40);                  /* BITMAPINFOHEADER */
	put_le32(bmp + 18, w);
	put_le32(bmp + 22, (uint32_t)-(int32_t)h); /* negative: rows top-down */
	put_le16(bmp + 26, 1);                   /* planes */
	put_le16(bmp + 28, 24);                  /* bits per pixel */
	put_le32(bmp + 34, (uint32_t)(row * h));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = rgba + (size_t)y * w * 4;
		uint8_t *d = bmp + 54 + (size_t)y * row;
		for (uint32_t x = 0; x < w; x++, s += 4, d += 3) {
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
		}
	}

	char *url = bmalloc(sizeof(prefix) - 1 + (size + 2) / 3 * 4 + 1);
	memcpy(url, prefix, sizeof(prefix) - 1);
	char *o = url + sizeof(prefix) - 1;
	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		uint32_t v = (uint32_t)bmp[i] << 16 | (uint32_t)bmp[i + 1] << 8 | bmp[i + 2];
		*o++ = b64[v >> 18];
		*o++ = b64[(v >> 12) & 63];
		*o++ = b64[(v >> 6) & 63];
		*o++ = b64[v & 63];
	}
	if (i < size) {
		uint32_t v = (uint32_t)bmp[i] << 16 | (i + 1 < size ? (uint32_t)bmp[i + 1] << 8 : 0);
		*o++ = b64[v >> 18];
		*o++ = b64[(v >> 12) & 63];
		*o++ = i + 1 < size ? b64[(v >> 6) & 63] : '=';
		*o++ = '=';
	}
	*o = '\0';
	bfree(bmp);
	return url;
}

/* One request per filter at a time (two editors, or overlapping polls). */
static bool claim_raw(struct handcam_pin *f)
{
	for (int i = 0; i < 1000; i++) {
		if (!atomic_exchange(&f->raw_busy, true))
			return true;
		os_sleep_ms(2);
	}
	return false;
}

/* Wait until hp_render has done the armed capture, or the timeout passes. */
static bool wait_for_capture(struct handcam_pin *f, uint32_t timeout_ms)
{
	uint64_t end = os_gettime_ns() + (uint64_t)timeout_ms * 1000000;
	while (!atomic_load(&f->raw_signal)) {
		if (os_gettime_ns() >= end)
			return false;
		os_sleep_ms(1);
	}
	return true;
}

static void vendor_get_raw_frame(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(priv);
	const char *error = NULL;
	uint8_t *pixels = NULL;
	uint32_t w = 0, h = 0, sw = 0, sh = 0;

	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "source"));
	obs_source_t *filter = source ? obs_source_get_filter_by_name(source, obs_data_get_string(request, "filter"))
				      : NULL;
	struct handcam_pin *f = NULL;
	if (filter && strcmp(obs_source_get_unversioned_id(filter), FILTER_ID) == 0)
		f = obs_obj_get_data(filter);

	long long max = obs_data_get_int(request, "max_size");
	uint32_t ow = source ? obs_source_get_width(source) : 0;
	uint32_t oh = source ? obs_source_get_height(source) : 0;

	if (!source) {
		error = "No source by that name.";
	} else if (!f) {
		error = "That source has no Handcam Pin filter by that name.";
	} else if (!obs_source_enabled(filter)) {
		error = "The Handcam Pin filter is switched off.";
	} else if (!ow || !oh) {
		error = "The source has no video yet.";
	} else if (!claim_raw(f)) {
		error = "Busy with another request, try again.";
	} else {
		uint32_t frame_ms = (uint32_t)(obs_get_frame_interval_ns() / 1000000) + 1;

		/* Arm the capture and let OBS's own render of this frame do it. */
		atomic_store(&f->raw_signal, false);
		obs_enter_graphics();
		f->raw_max = max <= 0 ? 0 : (uint32_t)(max < 64 ? 64 : max > 8192 ? 8192 : max);
		f->raw_done = false;
		f->raw_want = true;
		obs_leave_graphics();

		if (!wait_for_capture(f, 3 * frame_ms)) {
			/* Nothing is drawing the source (not in any visible scene), so
			 * the cached filter textures are stale anyway: render it
			 * ourselves. Only our capture matters; the image goes to 2x2 px. */
			obs_enter_graphics();
			if (f->raw_want) {
				gs_texrender_t *sink = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
				if (sink && gs_texrender_begin(sink, 2, 2)) {
					gs_ortho(0.0f, (float)ow, 0.0f, (float)oh, -100.0f, 100.0f);
					obs_source_video_render(source);
					gs_texrender_end(sink);
				}
				if (sink)
					gs_texrender_destroy(sink);
				gs_flush();
			}
			obs_leave_graphics();
		}

		/* Map a frame later, so the GPU has finished the copy by then and
		 * mapping doesn't stall OBS's render thread waiting for it. */
		os_sleep_ms(frame_ms);

		obs_enter_graphics();
		f->raw_want = false;
		uint8_t *data;
		uint32_t linesize;
		if (f->raw_done && gs_stagesurface_map(f->raw_stage, &data, &linesize)) {
			w = f->raw_w;
			h = f->raw_h;
			sw = f->raw_sw;
			sh = f->raw_sh;
			pixels = bmalloc((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++)
				memcpy(pixels + (size_t)y * w * 4, data + (size_t)y * linesize, (size_t)w * 4);
			gs_stagesurface_unmap(f->raw_stage);
		}
		f->raw_done = false;
		obs_leave_graphics();
		atomic_store(&f->raw_busy, false);

		if (!pixels)
			error = "Could not capture the camera image.";
	}

	if (pixels) {
		char *url = rgba_to_bmp_url(pixels, w, h);
		obs_data_set_string(response, "image", url);
		obs_data_set_int(response, "width", w);
		obs_data_set_int(response, "height", h);
		obs_data_set_int(response, "source_width", sw);
		obs_data_set_int(response, "source_height", sh);
		bfree(url);
		bfree(pixels);
	} else {
		obs_data_set_string(response, "error", error);
	}
	obs_source_release(filter);
	obs_source_release(source);
}

/* Register GetRawFrame with obs-websocket (in post_load, as its API requires).
 * Never unregistered: obs-websocket drops vendors itself when it unloads, and
 * at our unload it may already be gone. */
static void register_vendor_requests(void)
{
	calldata_t cd = {0};
	proc_handler_t *ws = NULL;
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd))
		ws = calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	if (!ws) {
		blog(LOG_INFO, LOG_PREFIX "obs-websocket not found; the editor's live raw view is unavailable");
		return;
	}

	cd = (calldata_t){0};
	calldata_set_string(&cd, "name", VENDOR_NAME);
	proc_handler_call(ws, "vendor_register", &cd);
	void *vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);

	bool ok = false;
	if (vendor) {
		struct ws_request_callback cb = {vendor_get_raw_frame, NULL};
		cd = (calldata_t){0};
		calldata_set_ptr(&cd, "vendor", vendor);
		calldata_set_string(&cd, "type", "GetRawFrame");
		calldata_set_ptr(&cd, "callback", &cb);
		proc_handler_call(ws, "vendor_request_register", &cd);
		ok = calldata_bool(&cd, "success");
		calldata_free(&cd);
	}
	if (!ok)
		blog(LOG_WARNING, LOG_PREFIX "could not register obs-websocket vendor request");
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

/* After all modules, so obs-websocket has written its config. */
void obs_module_post_load(void)
{
	bfree(write_live_editor());
	char *path = editor_path();
	editor_url = path ? path_to_file_url(path) : NULL;
	bfree(path);
	register_vendor_requests();
}

void obs_module_unload(void)
{
	bfree(editor_url);
	editor_url = NULL;
}
