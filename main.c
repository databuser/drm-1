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

struct gl_data {
	GLuint program;
	GLvoid *planes[3];
	struct egl_data *egl;
};

struct gbm_drm_data {
	struct gl_data *gl;
	struct gbm_bo *bo;
	uint32_t buf_id;
};

void _gbm_drm_remove_buf(struct gbm_bo *bo, void *user_data)
{
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)user_data;
	assert(gbm_drm);

	drmModeRmFB(
		gbm_drm->gl->egl->gbm->drm->fd,
		gbm_drm->buf_id);

	free(gbm_drm);
}

struct gbm_drm_data *gbm_drm_get_buf(struct gl_data *gl)
{
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(gl->egl->gbm->surface);
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)gbm_bo_get_user_data(bo);

	if (!gbm_drm) {
		gbm_drm = (struct gbm_drm_data *)calloc(1, sizeof(struct gbm_drm_data));
		gbm_drm->gl = gl;
		gbm_drm->bo = bo;

		int res = drmModeAddFB(
			gl->egl->gbm->drm->fd,
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

void drm_uninit(struct drm_data *drm)
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

void gl_draw(struct gl_data *gl)
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
return;
	struct gbm_drm_data *gbm_drm = (struct gbm_drm_data *)user_data;
	assert(gbm_drm);

	gbm_surface_release_buffer(gbm_drm->gl->egl->gbm->surface, gbm_drm->bo);

	gl_draw(gbm_drm->gl);
	eglSwapBuffers(gbm_drm->gl->egl->display, gbm_drm->gl->egl->surface);
	gbm_drm = gbm_drm_get_buf(gbm_drm->gl);

	drmModePageFlip(
		gbm_drm->gl->egl->gbm->drm->fd,
		gbm_drm->gl->egl->gbm->drm->crtc_id,
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

GLuint gl_compile_shader(GLenum type, const char *source)
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

GLuint gl_link_program(GLuint vertex_shader, GLuint fragment_shader)
{
	GLuint program = glCreateProgram();
	assert(program);

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	glLinkProgram(program);

	GLint linked;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	assert(linked);

	glUseProgram(program);
	glDeleteProgram(program);

	return program;
}

GLfloat *gl_get_vertices(
	const double wv, // view width
	const double hv, // view height
	const double wt, // texture width
	const double ht  // texture height
)
{
	static GLfloat vertices[8];

	double w = 2, h = 2;

	// ratios
	double wr = wt / wv, hr = ht / hv;

	if (wr > hr)
		h = ht / wr / hv * 2.0;
	else
		w = wt / hr / wv * 2.0;

	// 0, 1, 2, 3
	// 4, 5, 6, 7
	vertices[0] = vertices[4] = w / -2.0;
	vertices[5] = vertices[7] = h /  2.0;
	vertices[2] = vertices[6] = w /  2.0;
	vertices[1] = vertices[3] = h / -2.0;

	printf("View vertices:");
	for (int i = 0; i < 8; i++) {
		if (!(i % 4))
			printf("\n\t");
		printf("%05.3f, ", vertices[i]);
	}
	printf("\n");

	return &vertices[0];
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

struct gl_data *gl_init(struct egl_data *egl)
{
	static struct gl_data gl = { 0 };

	static const GLchar vertex_shader_source[] =
		"attribute vec4 a_position;                                  "
		"attribute vec2 a_coordinates;                               "
		"varying vec2 v_coordinates;                                 "
		"                                                            "
		"void main() {                                               "
		"  v_coordinates = a_coordinates;                            "
		"  gl_Position = a_position;                                 "
		"}                                                           ";

	static const GLchar fragment_shader_source[] =
		"precision highp float;                                      "
		"uniform sampler2D u_texture_y;                              "
		"uniform sampler2D u_texture_u;                              "
		"uniform sampler2D u_texture_v;                              "
		"varying vec2 v_coordinates;                                 "
		"uniform mat3 u_yuv_to_rgb;                                  "
		"                                                            "
		"void main() {                                               "
		"  float y = texture2D(u_texture_y, v_coordinates).r - 0.06; "
		"  float u = texture2D(u_texture_u, v_coordinates).r - 0.5;  "
		"  float v = texture2D(u_texture_v, v_coordinates).r - 0.5;  "
		"  vec3 rgb = u_yuv_to_rgb * vec3(y, u, v);                  "
		"  gl_FragColor = vec4(rgb, 1.0);                            "
		"}                                                           ";

	GLuint vertex_shader = gl_compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
	assert(vertex_shader != -1);

	GLuint fragment_shader = gl_compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
	assert(fragment_shader != -1);

	GLuint program = gl_link_program(vertex_shader, fragment_shader);

	// view (implicit aspect)
	const int wv = 1920;
	const int hv = 1080;
	// texture
	const int wt = 1920;
	const int ht = 1080;

	{
		GLfloat *vertices = gl_get_vertices(wv, hv, wt, ht);
/*
		static const GLfloat vertices[] = {
			-1, -1,  1, -1,
			-1,  1,  1,  1,
		};
*/

		GLint a_position = glGetAttribLocation(program, "a_position");
		glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
		glEnableVertexAttribArray(a_position);
	}

	{
/*
		static const GLfloat coordinates[] = {
			0.0, 1.0, 1.0, 1.0,
			0.0, 0.0, 1.0, 0.0,
		};
*/

		int w = 1920, h = 1080;
		int hcrop = 0;
		double aspect = w / (double)h;

		GLfloat yc = (h - hcrop) / (double)h;
		GLfloat xc = (w - hcrop * aspect) / (double)w;

		GLfloat x1 = 1.0 - xc;
		GLfloat y1 = yc;

		GLfloat x2 = xc;
		GLfloat y2 = 1.0 - yc;

		static GLfloat coordinates[8];
		coordinates[0] = x1;
		coordinates[1] = y1;
		coordinates[2] = x2;
		coordinates[3] = y1;
		coordinates[4] = x1;
		coordinates[5] = y2;
		coordinates[6] = x2;
		coordinates[7] = y2;

		GLint a_coordinates = glGetAttribLocation(program, "a_coordinates");
		glVertexAttribPointer(a_coordinates, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
		glEnableVertexAttribArray(a_coordinates);
	}

	{
		const GLfloat yuv_to_rgb[] = {
			//  B       G       R
			1.000,  1.000,  1.000, // Y
			1.772, -0.344,  0.000, // V
			0.000, -0.714,  1.402, // U
		};

		glUniformMatrix3fv(
			glGetUniformLocation(program, "u_yuv_to_rgb"),
			1,
			GL_FALSE,
			(GLfloat *)&yuv_to_rgb[0]);

		int w = 1920, h = 1080; // frame.yuv
		int y = w * h;
		int v = y / 4;
		int u = v;

		GLvoid *planes[] = {
			(GLvoid *)malloc(y),
			(GLvoid *)malloc(u),
			(GLvoid *)malloc(v),
		};

		gl.planes[0] = planes[0];
		gl.planes[1] = planes[1];
		gl.planes[2] = planes[2];

		{
			FILE *fp = fopen("frame.yuv", "rb");

			fseek(fp, 0, SEEK_END);
			int fs = ftell(fp);
			int frame_size = y + u + v;

			fseek(fp, fs % frame_size, SEEK_SET);
			fread(planes[0], y, 1, fp);
			fread(planes[2], v, 1, fp);
			fread(planes[1], u, 1, fp);

			fclose(fp);
		}

		GLuint textures[3];
		glGenTextures(3, textures);

		// Y
		GLint u_texture_y = glGetUniformLocation(program, "u_texture_y");
		glUniform1i(u_texture_y, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, u_texture_y);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(
			GL_TEXTURE_2D,
			0,
			GL_LUMINANCE,
			w,
			h,
			0,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			(GLvoid *)planes[0]);

		// U
		GLint u_texture_u = glGetUniformLocation(program, "u_texture_u");
		glUniform1i(u_texture_u, 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, u_texture_u);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(
			GL_TEXTURE_2D,
			0,
			GL_LUMINANCE,
			w / 2,
			h / 2,
			0,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			(GLvoid *)planes[1]);

		// V
		GLint u_texture_v = glGetUniformLocation(program, "u_texture_v");
		glUniform1i(u_texture_v, 2);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, u_texture_v);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(
			GL_TEXTURE_2D,
			0,
			GL_LUMINANCE,
			w / 2,
			h / 2,
			0,
			GL_LUMINANCE,
			GL_UNSIGNED_BYTE,
			(GLvoid *)planes[2]);
/*
		free(planes[2]);
		free(planes[1]);
		free(planes[0]);
*/
		glViewport(
			0,
			0,
			egl->gbm->drm->mode->hdisplay,
			egl->gbm->drm->mode->vdisplay);
	}

	gl.program = program;
	gl.egl = egl;

	return &gl;
}

int main(int argc, char *argv[])
{
	const char name[] = "/dev/dri/card0";

	struct drm_data *drm = drm_init(name);
	struct gbm_data *gbm = gbm_init(drm);
	struct egl_data *egl = egl_init(gbm);
	struct gl_data *gl = gl_init(egl);

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

	printf(
		"Using EGL version %d.%d.\n",
		egl->major,
		egl->minor);

	{
		gl_draw(gl);
		eglSwapBuffers(egl->display, egl->surface);
		struct gbm_drm_data *gbm_drm = gbm_drm_get_buf(gl);

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
	}

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

	drm_uninit(drm);

	return 0;
}
