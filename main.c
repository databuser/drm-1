#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gbm.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

struct drm_data {
	int fd;
	drmVersion *version;
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	drmModeCrtc *crtc;
	uint32_t crtc_id;
};

struct gbm_data {
	struct gbm_device *device;
	struct gbm_surface *surface;
	struct gbm_bo *bo;
	struct drm_data *drm;
};

struct egl_data {
	EGLint major, minor;
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	struct gbm_data *gbm;
};

struct gbm_drm_data {
	struct egl_data *egl;

	struct gbm_bo *bo;
	uint32_t buf_id;
};

void _gbm_drm_remove_buf(struct gbm_bo *bo, void *user_data)
{
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)user_data;
	assert(gbm_drm);

	drmModeRmFB(
		gbm_drm->egl->gbm->drm->fd,
		gbm_drm->buf_id);

	free(gbm_drm);
}

struct gbm_drm_data *gbm_drm_get_buf(struct egl_data *egl)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(egl->gbm->surface);
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)gbm_bo_get_user_data(bo);

	if (!gbm_drm) {
		gbm_drm = (struct gbm_drm_data *)calloc(1, sizeof(struct gbm_drm_data));
		gbm_drm->egl = egl;
		gbm_drm->bo = bo;

		int res = drmModeAddFB(
			egl->gbm->drm->fd,
			gbm_bo_get_width(bo),
			gbm_bo_get_height(bo),
			24,
			32,
			gbm_bo_get_stride(bo),
			gbm_bo_get_handle(bo).u32,
			&gbm_drm->buf_id);
		assert(res == 0);

		gbm_bo_set_user_data(
			bo,
			(void *)gbm_drm,
			_gbm_drm_remove_buf);
	}

	return gbm_drm;
}

struct drm_data *drm_init(const char *name)
{
	static struct drm_data drm = { 0 };

	drm.fd = open(name, O_RDWR | O_CLOEXEC);
	assert(drm.fd >= 0);

	drm.version = drmGetVersion(drm.fd);
	assert(drm.version);

	drmModeRes *resources = drmModeGetResources(drm.fd);
	assert(resources);

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			drm.connector = connector;
//			printf("Using connector %d.\n", connector->connector_id);
//			break;
		}
	}
	assert(drm.connector);
	assert(drm.connector->count_modes > 0);

	for (int i = 0; i < drm.connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, drm.connector->encoders[i]);
		if (!encoder)
			continue;

		for (int j = 0; j < resources->count_crtcs; j++) {
			if (encoder->possible_crtcs & (1 << j)) {
				drm.crtc_id = resources->crtcs[j];
//				printf("Using crtc %d.\n", resources->crtcs[j]);
//				break;
			}
		}

		drmModeFreeEncoder(encoder);

		if (drm.crtc_id)
			break;
	}

	static drmModeModeInfo mode = {
		15150,
		1900, 1900, 1900, 1900, 0,
		963,   963,  963,  963, 0,
		60,
		6,
		64,
		"1900x963"
	};
	drm.mode = &mode;
//	printf("Using mode %s.\n", mode.name);

	drm.crtc = drmModeGetCrtc(drm.fd, drm.crtc_id);
	assert(drm.crtc);

	return &drm;
}

void drm_deinit(struct drm_data *drm)
{
	assert(drm);

	drmModeFreeCrtc(drm->crtc);

	assert(drm->connector);
	drmModeFreeConnector(drm->connector);

	assert(drm->version);
	drmFreeVersion(drm->version);

	assert(drm->fd);
	close(drm->fd);
}

void gl_draw()
{
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void page_flip_handler(
	int fd,
	unsigned int sequence,
	unsigned int tv_sec,
	unsigned int tv_usec,
	void *user_data)
{
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)user_data;
	assert(gbm_drm);

	gbm_surface_release_buffer(gbm_drm->egl->gbm->surface, gbm_drm->bo);

	gl_draw();
	eglSwapBuffers(gbm_drm->egl->display, gbm_drm->egl->surface);
	gbm_drm = gbm_drm_get_buf(gbm_drm->egl);

	drmModePageFlip(
		gbm_drm->egl->gbm->drm->fd,
		gbm_drm->egl->gbm->drm->crtc_id,
		gbm_drm->buf_id,
		DRM_MODE_PAGE_FLIP_EVENT,
		gbm_drm);
}

struct gbm_data *gbm_init(struct drm_data *drm)
{
	static struct gbm_data gbm = { 0 };

	gbm.device = gbm_create_device(drm->fd);
	gbm.surface = gbm_surface_create(
		gbm.device,
		drm->mode->hdisplay,
		drm->mode->vdisplay,
		GBM_FORMAT_XRGB8888,
		GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	assert(gbm.surface);

	gbm.drm = drm;

	return &gbm;
}

GLuint gl_load_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	assert(shader);

	glShaderSource(shader, 1, &source, 0);
	glCompileShader(shader);

	GLint compiled;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	assert(compiled);

	return shader;
}

struct egl_data *egl_init(struct gbm_data *gbm)
{
	static struct egl_data egl = { 0 };
	egl.gbm = gbm;

	{
		egl.display = eglGetDisplay(gbm->device);
		assert(egl.display != EGL_NO_DISPLAY);
	}

	{
		EGLBoolean res = eglInitialize(egl.display, &egl.major, &egl.minor);
		assert(res != EGL_FALSE);
	}

	{
		EGLBoolean res = eglBindAPI(EGL_OPENGL_ES_API);
		assert(res != EGL_FALSE);
	}

	{
		static const EGLint attrib_list[] = {
			EGL_BLUE_SIZE,       8,
			EGL_GREEN_SIZE,      8,
			EGL_RED_SIZE,        8,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
			EGL_NONE
		};

		EGLint num_config;
		EGLBoolean res = eglChooseConfig(
			egl.display,
			attrib_list,
			&egl.config,
			1,
			&num_config);
		assert(res != EGL_FALSE);
	}

	{
		static const EGLint attrib_list[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		};

		egl.context = eglCreateContext(
			egl.display,
			egl.config,
			EGL_NO_CONTEXT,
			attrib_list);
		assert(egl.context != EGL_NO_CONTEXT);
	}

	{
		egl.surface = eglCreateWindowSurface(
			egl.display,
			egl.config,
			(NativeWindowType)gbm->surface,
			0);
		assert(egl.surface != EGL_NO_SURFACE);
	}

	{
		EGLBoolean res = eglMakeCurrent(
			egl.display,
			egl.surface,
			egl.surface,
			egl.context);
		assert(res != EGL_FALSE);
	}

	return &egl;
}

int main(int argc, char *argv[])
{
	const char name[] = "/dev/dri/card0";

	struct drm_data *drm = drm_init(name);
	struct gbm_data *gbm = gbm_init(drm);
	struct egl_data *egl = egl_init(gbm);

	{
		static const GLchar vertex_shader_source[] =
			"attribute vec4 a_position;           "
			"attribute vec3 a_color;              "
			"varying vec3 v_color;                "
			"                                     "
			"void main() {                        "
			"  gl_Position = a_position;          "
			"  v_color = a_color;                 "
			"}                                    ";

		static const GLchar fragment_shader_source[] =
			"precision mediump float;             "
			"varying vec3 v_color;                "
			"                                     "
			"void main() {                        "
			"  gl_FragColor = vec4(v_color, 1.0); "
			"}                                    ";

		GLuint vertex_shader = gl_load_shader(GL_VERTEX_SHADER, vertex_shader_source);
		assert(vertex_shader != -1);

		GLuint fragment_shader = gl_load_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
		assert(fragment_shader != -1);

		GLuint program = glCreateProgram();

		glAttachShader(program, vertex_shader);
		glAttachShader(program, fragment_shader);

		glLinkProgram(program);
		GLint linked;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		assert(linked);

		glUseProgram(program);

		{
			static const GLfloat vertices[] = {
				-1.0,  1.0,  1.0,  1.0,
				-1.0, -1.0,  1.0, -1.0,
			};

			GLint position = glGetAttribLocation(program, "a_position");
			glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, &vertices[0]);
			glEnableVertexAttribArray(position);
		}

		{
			static const GLfloat colors[] = {
				1.0, 0.0, 0.0, 0.0, 1.0, 0.0,
				0.0, 0.0, 1.0, 1.0, 1.0, 0.0,
			};

			GLint color = glGetAttribLocation(program, "a_color");
			glVertexAttribPointer(color, 3, GL_FLOAT, GL_FALSE, 0, &colors[0]);
			glEnableVertexAttribArray(color);
		}
	}

	printf("Using EGL version %d.%d.\n", egl->major, egl->minor);

	printf("Using %s.\n", name);
	printf(
		"Version: %d.%d.%d\n",
		drm->version->version_major,
		drm->version->version_minor,
		drm->version->version_patchlevel
	);
	printf("Date: %s\n", drm->version->date);
	printf("Name: %s\n", drm->version->name);
	printf("Description: %s\n", drm->version->desc);

	gl_draw();
	eglSwapBuffers(egl->display, egl->surface);
	struct gbm_drm_data *gbm_drm = gbm_drm_get_buf(egl);

	int res = drmModeSetCrtc(
		drm->fd,
		drm->crtc_id,
		gbm_drm->buf_id,
		0,
		0,
		&drm->connector->connector_id,
		1,
		drm->mode);
	assert(res == 0);

	drmModePageFlip(
		drm->fd,
		drm->crtc_id,
		gbm_drm->buf_id,
		DRM_MODE_PAGE_FLIP_EVENT,
		gbm_drm);

	fd_set fds;
	FD_ZERO(&fds);

	drmEventContext ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.vblank_handler = 0,
		.page_flip_handler = page_flip_handler
	};

	const long double nsec = 1000000000;
	struct timespec t1, t2;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	uint32_t f = 0;

	while (1) {
		FD_SET(0, &fds);
		FD_SET(drm->fd, &fds);

		int res = select(drm->fd + 1, &fds, 0, 0, 0);
		assert(res >= 0);

		if (FD_ISSET(0, &fds))
			break;
		else if (FD_ISSET(drm->fd, &fds))
			drmHandleEvent(drm->fd, &ev);

		{
			struct timespec t3 = t2;
			clock_gettime(CLOCK_MONOTONIC, &t2);
			long double e1 = t2.tv_sec - t1.tv_sec + (t2.tv_nsec - t1.tv_nsec) / nsec;
			long double e2 = t2.tv_sec - t3.tv_sec + (t2.tv_nsec - t3.tv_nsec) / nsec;
			fprintf(
				stdout,
				"F: %d, E: %.9Lf, A: %.9Lf, B: %d, C: %.9Lf        \r",
				f,
				e1,
				f / e1,
				0,
				1 / e2);
			fflush(stdout);
		}

		++f;
	}

	drmModeSetCrtc(
		drm->fd,
		drm->crtc->crtc_id,
		drm->crtc->buffer_id,
		drm->crtc->x,
		drm->crtc->y,
		&drm->connector->connector_id,
		1,
		&drm->crtc->mode);

	drm_deinit(drm);

	return 0;
}
