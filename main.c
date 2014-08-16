#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

struct drm_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct drm_device {
	int fd;
	drmVersion *version;
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	drmModeCrtc *crtc;
	uint32_t crtc_id;
	struct drm_buf buf;
};

void drm_colorize(struct drm_device *dev)
{
	static int16_t n = 0;
	static uint8_t d = 0;

	if (d) {
		if (++n > 255) {
			d ^= 1;
			n -= 2;
		}
	}
	else {
		if (--n < 0) {
			d ^= 1;
			n += 2;
		}
	}
/*
	time_t t;
	srand(time(&t));
	uint8_t r = rand() % 0xff, g = rand() % 0xff, b = rand() % 0xff;
*/
	uint8_t r = 0, g = 0, b = n;

	for (int i = 0; i < dev->buf.height; i++)
		for (int j = 0; j < dev->buf.width; j++)
			*(uint32_t *)&dev->buf.map[dev->buf.stride * i + j * 4] =
				(r << 16) | (g << 8) | b;
}

struct drm_device *drm_init(const char *name)
{
	static struct drm_device dev = { 0 };

	dev.fd = open(name, O_RDWR | O_CLOEXEC);
	assert(dev.fd >= 0);

	dev.version = drmGetVersion(dev.fd);
	assert(dev.version);

	drmModeRes *resources = drmModeGetResources(dev.fd);
	assert(resources);

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector = drmModeGetConnector(dev.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			dev.connector = connector;
			printf("Using connector %d.\n", connector->connector_id);
//			break;
		}
	}
	assert(dev.connector);
	assert(dev.connector->count_modes > 0);

	for (int i = 0; i < dev.connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(dev.fd, dev.connector->encoders[i]);
		if (!encoder)
			continue;

		for (int j = 0; j < resources->count_crtcs; j++) {
			if (encoder->possible_crtcs & (1 << j)) {
				dev.crtc_id = resources->crtcs[j];
				printf("Using crtc %d.\n", resources->crtcs[j]);
//				break;
			}
		}

		drmModeFreeEncoder(encoder);

		if (dev.crtc_id)
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
	dev.mode = &mode;
	printf("Using mode %s.\n", mode.name);

/*
	for (int i = 0; i < dev.con->count_modes; i++) {
		printf("[%s]\n", dev.con->modes[i].name);
		printf("\tclock = %d\n", dev.con->modes[i].clock);
		printf("\tvrefresh = %d\n", dev.con->modes[i].vrefresh);
		printf("\tflags = %d\n", dev.con->modes[i].flags);
		printf("\ttype = %d\n", dev.con->modes[i].type);
		printf(
			"\t%4d %4d %4d %4d %4d\n",
			dev.con->modes[i].hdisplay,
			dev.con->modes[i].hsync_start,
			dev.con->modes[i].hsync_end,
			dev.con->modes[i].htotal,
			dev.con->modes[i].hskew
		);
		printf(
			"\t%4d %4d %4d %4d %4d\n",
			dev.con->modes[i].vdisplay,
			dev.con->modes[i].vsync_start,
			dev.con->modes[i].vsync_end,
			dev.con->modes[i].vtotal,
			dev.con->modes[i].vscan
		);
	}
*/

	{
		struct drm_mode_create_dumb creq = { 0 };
		creq.width = mode.hdisplay;
		creq.height = mode.vdisplay;
		creq.bpp = 32;
		int res = drmIoctl(dev.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
		assert(res >= 0);

		dev.buf.width = creq.width;
		dev.buf.height = creq.height;
		dev.buf.stride = creq.pitch;
		dev.buf.size = creq.size;
		dev.buf.handle = creq.handle;

		res = drmModeAddFB(
			dev.fd,
			creq.width,
			creq.height,
			24,
			32,
			creq.pitch,
			creq.handle,
			&dev.buf.fb);
		assert(res == 0);

		struct drm_mode_map_dumb mreq = { 0 };
		mreq.handle = creq.handle;
		res = drmIoctl(dev.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
		assert(res == 0);

		dev.buf.map = mmap(
			0,
			creq.size,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			dev.fd,
			mreq.offset);
		assert(dev.buf.map != MAP_FAILED);

		memset(dev.buf.map, 0, dev.buf.size);
	}

	dev.crtc = drmModeGetCrtc(dev.fd, dev.crtc_id);
	assert(dev.crtc);

	return &dev;
}

void drm_deinit(struct drm_device *dev)
{
	assert(dev);

	drmModeFreeCrtc(dev->crtc);

	{
		munmap(dev->buf.map, dev->buf.size);
		drmModeRmFB(dev->fd, dev->buf.fb);
		struct drm_mode_destroy_dumb dreq = { 0 };
		dreq.handle = dev->buf.handle;
		drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}

	assert(dev->connector);
	drmModeFreeConnector(dev->connector);

	assert(dev->version);
	drmFreeVersion(dev->version);

	assert(dev->fd);
	close(dev->fd);
}

void page_flip_handler(
	int fd,
	unsigned int sequence,
	unsigned int tv_sec,
	unsigned int tv_usec,
	void *user_data)
{
	struct drm_device *dev = (struct drm_device *)user_data;

	drm_colorize(dev);
	drmModePageFlip(
		dev->fd,
		dev->crtc_id,
		dev->buf.fb,
		DRM_MODE_PAGE_FLIP_EVENT,
		dev);
}

int main(int argc, char *argv[])
{
	const char name[] = "/dev/dri/card0";
	struct drm_device *dev = drm_init(name);

	printf("Using %s.\n", name);
	printf(
		"Version: %d.%d.%d\n",
		dev->version->version_major,
		dev->version->version_minor,
		dev->version->version_patchlevel
	);
	printf("Date: %s\n", dev->version->date);
	printf("Name: %s\n", dev->version->name);
	printf("Description: %s\n", dev->version->desc);

	int res = drmModeSetCrtc(
		dev->fd,
		dev->crtc_id,
		dev->buf.fb,
		0,
		0,
		&dev->connector->connector_id,
		1,
		dev->mode);
	assert(res == 0);

	drm_colorize(dev);
	drmModePageFlip(
		dev->fd,
		dev->crtc_id,
		dev->buf.fb,
		DRM_MODE_PAGE_FLIP_EVENT,
		dev);

	fd_set fds;
	FD_ZERO(&fds);

	drmEventContext ev = {
		DRM_EVENT_CONTEXT_VERSION,
		0,
		page_flip_handler
	};

	const long double nsec = 1000000000;
	struct timespec t1, t2;
	clock_gettime(CLOCK_MONOTONIC, &t1);

	while (1) {
		FD_SET(0, &fds);
		FD_SET(dev->fd, &fds);

		res = select(dev->fd + 1, &fds, 0, 0, 0);
		assert(res >= 0);

		if (FD_ISSET(0, &fds))
			break;
		else if (FD_ISSET(dev->fd, &fds))
			drmHandleEvent(dev->fd, &ev);

		{
			struct timespec t3 = t2;
			clock_gettime(CLOCK_MONOTONIC, &t2);
			long double e1 = t2.tv_sec - t1.tv_sec + (t2.tv_nsec - t1.tv_nsec) / nsec;
			long double e2 = t2.tv_sec - t3.tv_sec + (t2.tv_nsec - t3.tv_nsec) / nsec;
			fprintf(stdout, "%.9Lf, %.9Lf        \r", e1, 1 / e2);
			fflush(stdout);
		}
	}

	res = drmModeSetCrtc(
		dev->fd,
		dev->crtc->crtc_id,
		dev->crtc->buffer_id,
		dev->crtc->x,
		dev->crtc->y,
		&dev->connector->connector_id,
		1,
		&dev->crtc->mode);

	drm_deinit(dev);

	return 0;
}
