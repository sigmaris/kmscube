/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include "esUtil.h"


#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define VSYNC 1

static struct {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint program;
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLuint vbo;
	GLuint positionsoffset, colorsoffset, normalsoffset;
} gl;

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface;
} gbm;

static struct {
	int fd;
	drmModeModeInfo *mode;
	drmModeCrtc *saved_crtc;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static void print_fds(void)
{
	DIR *d;
	const int maxlength = 256;
	char path[maxlength];

	snprintf(path, maxlength, "/proc/%i/fd/", getpid());

	struct dirent *dir;
	d = opendir(path);
	if (d) {
		while((dir = readdir(d)) != NULL) {
			if (dir->d_type == DT_LNK) {
				char hardfile[maxlength];
				ssize_t len;
				char tempath[maxlength];

				snprintf(tempath, maxlength, "%s%s", path, dir->d_name);
				len = readlink(tempath, hardfile, maxlength - 1);
				if (len != -1) {
					hardfile[len] = '\0';
					printf("%s -> %s\n", dir->d_name, hardfile);
				} else {
					printf("error when executing readlink() on %s\n",tempath);
				}
			}
		}
		closedir(d);
	}
}

static int init_drm(void)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, j, area;

	drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

	if (drm.fd < 0) {
		printf("could not open drm device\n");
		return -1;
	}

	printf("After open(), fds:\n");
	print_fds();

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		printf("connector %u current encoder %u\n", connector->connector_id, connector->encoder_id);
		printf("connector %u can use encoders:", connector->connector_id);
		for (j = 0, j < connector->count_encoders; j++) {
			printf("%u, ", connector->encoders[j]);
		}
		printf("\n");
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED
			&& connector->count_modes > 0) {
			/* it's connected and has modes, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

	/* find highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];
		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		printf("could not find mode!\n");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		printf("encoder %u has CRTC %u\n", encoder->encoder_id, encoder->crtc_id);
		printf("encoder %u possible CRTCs: %x\n", encoder->encoder_id, encoder->possible_crtcs);
		printf("encoder %u possible clones: %x\n", encoder->encoder_id, encoder->possible_clones);
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (!encoder) {
		printf("no encoder!\n");
		return -1;
	}

	drm.crtc_id = encoder->crtc_id;
	drm.connector_id = connector->connector_id;
	drm.saved_crtc = drmModeGetCrtc(drm.fd, encoder->crtc_id);

	return 0;
}

static int deinit_drm(void)
{
	int ret;
	// Restore original CRTC settings
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, drm.saved_crtc->buffer_id,
			drm.saved_crtc->x, drm.saved_crtc->y,
			&drm.connector_id, 1, &drm.saved_crtc->mode);
	if (ret) {
		printf("warning: failed to restore original mode: %s\n", strerror(errno));
	}
	drmModeFreeCrtc(drm.saved_crtc);
	ret = close(drm.fd);
	if (ret) {
		printf("failed to close DRM fd: %s\n", strerror(errno));
		return ret;
	}
	printf("After close(), fds:\n");
	print_fds();

	return 0;
}

static int init_gbm(void)
{
	gbm.dev = gbm_create_device(drm.fd);
	printf("After gbm_create_device(), fds:\n");
	print_fds();

	gbm.surface = gbm_surface_create(gbm.dev,
			drm.mode->hdisplay, drm.mode->vdisplay,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm.surface) {
		printf("failed to create gbm surface\n");
		return -1;
	}
	printf("After gbm_surface_create(), fds:\n");
	print_fds();

	return 0;
}

static void deinit_gbm(void)
{
	gbm_surface_destroy(gbm.surface);
	printf("After gbm_surface_destroy(), fds:\n");
	print_fds();

	gbm_device_destroy(gbm.dev);
	printf("After gbm_device_destroy(), fds:\n");
	print_fds();
}

static int init_gl(void)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;

	static const GLfloat vVertices[] = {
			// front
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f, // point magenta
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			// back
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, +1.0f, -1.0f, // point yellow
			-1.0f, +1.0f, -1.0f, // point green
			// right
			+1.0f, -1.0f, +1.0f, // point magenta
			+1.0f, -1.0f, -1.0f, // point red
			+1.0f, +1.0f, +1.0f, // point white
			+1.0f, +1.0f, -1.0f, // point yellow
			// left
			-1.0f, -1.0f, -1.0f, // point black
			-1.0f, -1.0f, +1.0f, // point blue
			-1.0f, +1.0f, -1.0f, // point green
			-1.0f, +1.0f, +1.0f, // point cyan
			// top
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			-1.0f, +1.0f, -1.0f, // point green
			+1.0f, +1.0f, -1.0f, // point yellow
			// bottom
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f  // point magenta
	};

	static const GLfloat vColors[] = {
			// front
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f, // magenta
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			// back
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  0.0f, // black
			1.0f,  1.0f,  0.0f, // yellow
			0.0f,  1.0f,  0.0f, // green
			// right
			1.0f,  0.0f,  1.0f, // magenta
			1.0f,  0.0f,  0.0f, // red
			1.0f,  1.0f,  1.0f, // white
			1.0f,  1.0f,  0.0f, // yellow
			// left
			0.0f,  0.0f,  0.0f, // black
			0.0f,  0.0f,  1.0f, // blue
			0.0f,  1.0f,  0.0f, // green
			0.0f,  1.0f,  1.0f, // cyan
			// top
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			0.0f,  1.0f,  0.0f, // green
			1.0f,  1.0f,  0.0f, // yellow
			// bottom
			0.0f,  0.0f,  0.0f, // black
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f  // magenta
	};

	static const GLfloat vNormals[] = {
			// front
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			// back
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			// right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			// left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			// top
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			// bottom
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f  // down
	};

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	static const char *vertex_shader_source =
			"uniform mat4 modelviewMatrix;      \n"
			"uniform mat4 modelviewprojectionMatrix;\n"
			"uniform mat3 normalMatrix;         \n"
			"                                   \n"
			"attribute vec4 in_position;        \n"
			"attribute vec3 in_normal;          \n"
			"attribute vec4 in_color;           \n"
			"\n"
			"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_Position = modelviewprojectionMatrix * in_position;\n"
			"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
			"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
			"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
			"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
			"    float diff = max(0.0, dot(vEyeNormal, vLightDir));\n"
			"    vVaryingColor = vec4(diff * in_color.rgb, 1.0);\n"
			"}                                  \n";

	static const char *fragment_shader_source =
			"precision mediump float;           \n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_FragColor = vVaryingColor;  \n"
			"}                                  \n";

	gl.display = eglGetDisplay(gbm.dev);
	printf("After eglGetDisplay(), fds:\n");
	print_fds();

	if (!eglInitialize(gl.display, &major, &minor)) {
		printf("failed to initialize\n");
		return -1;
	}
	printf("After eglInitialize(), fds:\n");
	print_fds();

	printf("Using display %p with EGL version %d.%d\n",
			gl.display, major, minor);

	printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
	printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
	printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("failed to bind api EGL_OPENGL_ES_API\n");
		return -1;
	}

	if (!eglChooseConfig(gl.display, config_attribs, &gl.config, 1, &n) || n != 1) {
		printf("failed to choose config: %d\n", n);
		return -1;
	}

	gl.context = eglCreateContext(gl.display, gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (gl.context == NULL) {
		printf("failed to create context\n");
		return -1;
	}

	printf("After eglCreateContext(), fds:\n");
	print_fds();

	gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface, NULL);
	if (gl.surface == EGL_NO_SURFACE) {
		printf("failed to create egl surface\n");
		return -1;
	}

	printf("After eglCreateWindowSurface(), fds:\n");
	print_fds();

	/* connect the context to the surface */
	eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);
	printf("After eglMakeCurrent(), fds:\n");
	print_fds();


	vertex_shader = glCreateShader(GL_VERTEX_SHADER);

	glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertex_shader);

	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("vertex shader compilation failed!:\n");
		glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &ret);
		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(vertex_shader, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragment_shader);

	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("fragment shader compilation failed!:\n");
		glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(fragment_shader, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	gl.program = glCreateProgram();

	glAttachShader(gl.program, vertex_shader);
	glAttachShader(gl.program, fragment_shader);

	glBindAttribLocation(gl.program, 0, "in_position");
	glBindAttribLocation(gl.program, 1, "in_normal");
	glBindAttribLocation(gl.program, 2, "in_color");

	glLinkProgram(gl.program);

	glGetProgramiv(gl.program, GL_LINK_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("program linking failed!:\n");
		glGetProgramiv(gl.program, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetProgramInfoLog(gl.program, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	glUseProgram(gl.program);

	gl.modelviewmatrix = glGetUniformLocation(gl.program, "modelviewMatrix");
	gl.modelviewprojectionmatrix = glGetUniformLocation(gl.program, "modelviewprojectionMatrix");
	gl.normalmatrix = glGetUniformLocation(gl.program, "normalMatrix");

	glViewport(0, 0, drm.mode->hdisplay, drm.mode->vdisplay);
	glEnable(GL_CULL_FACE);

	gl.positionsoffset = 0;
	gl.colorsoffset = sizeof(vVertices);
	gl.normalsoffset = sizeof(vVertices) + sizeof(vColors);
	glGenBuffers(1, &gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices) + sizeof(vColors) + sizeof(vNormals), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, gl.positionsoffset, sizeof(vVertices), &vVertices[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.colorsoffset, sizeof(vColors), &vColors[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.normalsoffset, sizeof(vNormals), &vNormals[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.positionsoffset);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.normalsoffset);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.colorsoffset);
	glEnableVertexAttribArray(2);

	return 0;
}

static int deinit_gl(void)
{
	int ret = 0;
	glDeleteBuffers(1, &gl.vbo);
	glDeleteProgram(gl.program);
	if(!eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		ret = (int)eglGetError();
		printf("eglMakeCurrent error: %d\n", ret);
		return ret;
	}
	if(!eglDestroySurface(gl.display, gl.surface)) {
		ret = (int)eglGetError();
		printf("eglDestroySurface error: %d\n", ret);
		return ret;
	}
	printf("After eglDestroySurface(), fds:\n");
	print_fds();
	if(!eglDestroyContext(gl.display, gl.context)) {
		ret = (int)eglGetError();
		printf("eglDestroyContext error: %d\n", ret);
		return ret;
	}
	printf("After eglDestroyContext(), fds:\n");
	print_fds();
	if(!eglTerminate(gl.display)) {
		ret = (int)eglGetError();
		printf("eglTerminate error: %d\n", ret);
		return ret;
	}
	printf("After eglTerminate(), fds:\n");
	print_fds();
	return ret;
}

static void draw(uint32_t i)
{
	ESMatrix modelview;

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -8.0f);
	esRotate(&modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

	GLfloat aspect = (GLfloat)(drm.mode->vdisplay) / (GLfloat)(drm.mode->hdisplay);

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 6.0f, 10.0f);

	ESMatrix modelviewprojection;
	esMatrixLoadIdentity(&modelviewprojection);
	esMatrixMultiply(&modelviewprojection, &modelview, &projection);

	float normal[9];
	normal[0] = modelview.m[0][0];
	normal[1] = modelview.m[0][1];
	normal[2] = modelview.m[0][2];
	normal[3] = modelview.m[1][0];
	normal[4] = modelview.m[1][1];
	normal[5] = modelview.m[1][2];
	normal[6] = modelview.m[2][0];
	normal[7] = modelview.m[2][1];
	normal[8] = modelview.m[2][2];

	glUniformMatrix4fv(gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniformMatrix3fv(gl.normalmatrix, 1, GL_FALSE, normal);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);

	printf("After drmModeRmFB(), fds:\n");
	print_fds();

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}
	printf("After drmModeAddFB(), fds:\n");
	print_fds();

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

static long timeval_diff(struct timeval *start, struct timeval *stop) {
	long secs = stop->tv_sec - start->tv_sec;
	long usecs = 0;
	if (stop->tv_usec > start->tv_usec) {
		usecs = stop->tv_usec - start->tv_usec;
	} else {
		usecs = start->tv_usec - stop->tv_usec;
	}
	return (1000000 * secs) + usecs;
}

int main(int argc, char *argv[])
{
	int ret;
	printf("Drawing 300 frames...\n");
	ret = draw_some_frames(300);
	if(ret) {
		printf("Exiting with error.\n");
		return ret;
	}
	printf("Sleeping...\n");
	sleep(10);
	printf("Drawing another 300 frames...\n");
	ret = draw_some_frames(300);
	if(ret) {
		printf("Exiting with error.\n");
	}
	return ret;
}

int draw_some_frames(uint32_t num_frames)
{
	fd_set fds;
	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	struct gbm_bo *bo;
	struct drm_fb *fb;
	uint32_t i = 0;
	int ret = 0;

	printf("before init_drm(), fds:\n");
	print_fds();
	ret = init_drm();
	if (ret) {
		printf("failed to initialize DRM\n");
		return ret;
	}

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(drm.fd, &fds);

	printf("before init_gbm(), fds:\n");
	print_fds();
	ret = init_gbm();
	if (ret) {
		printf("failed to initialize GBM\n");
		return ret;
	}

	printf("before init_gl(), fds:\n");
	print_fds();
	ret = init_gl();
	if (ret) {
		printf("failed to initialize EGL\n");
		return ret;
	}
	printf("After init_gl(), fds:\n");
	print_fds();

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(gl.display, gl.surface);
	bo = gbm_surface_lock_front_buffer(gbm.surface);
	printf("After gbm_surface_lock_front_buffer(), fds:\n");
	print_fds();

	fb = drm_fb_get_from_bo(bo);

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}
	printf("After first drmModeSetCrtc(), fds:\n");
	print_fds();

	long drawtime = 0, swaptime = 0, locktime = 0, fliptime = 0, releasetime = 0;
	struct timeval stop, start;
	int waiting_for_flip = 0;
	struct gbm_bo *next_bo;

	while (i < num_frames) {

		gettimeofday(&start, NULL);
		draw(i++);
		gettimeofday(&stop, NULL);
		drawtime = (drawtime + timeval_diff(&start, &stop)) / 2;

		gettimeofday(&start, NULL);
		eglSwapBuffers(gl.display, gl.surface);
		gettimeofday(&stop, NULL);
		swaptime = (swaptime + timeval_diff(&start, &stop)) / 2;

		gettimeofday(&start, NULL);
		next_bo = gbm_surface_lock_front_buffer(gbm.surface);
		gettimeofday(&stop, NULL);
		locktime = (locktime + timeval_diff(&start, &stop)) / 2;

		fb = drm_fb_get_from_bo(next_bo);

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */
		gettimeofday(&start, NULL);
		if (!VSYNC) {
			ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
					&drm.connector_id, 1, drm.mode);
			if (ret) {
				printf("failed to set new buffer: %s\n", strerror(errno));
				return -1;
			}
		} else {
			waiting_for_flip = 1;
			ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
			if (ret) {
				printf("failed to queue page flip: %s\n", strerror(errno));
				return -1;
			}

			while (waiting_for_flip) {
				ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
				if (ret < 0) {
					printf("select err: %s\n", strerror(errno));
					return ret;
				} else if (ret == 0) {
					printf("select timeout!\n");
					return -1;
				} else if (FD_ISSET(0, &fds)) {
					printf("user interrupted!\n");
					break;
				}
				drmHandleEvent(drm.fd, &evctx);
			}
		}
		gettimeofday(&stop, NULL);
		fliptime = (fliptime + timeval_diff(&start, &stop)) / 2;

		/* release last buffer to render on again: */
		gettimeofday(&start, NULL);
		gbm_surface_release_buffer(gbm.surface, bo);
		gettimeofday(&stop, NULL);
		releasetime = (releasetime + timeval_diff(&start, &stop)) / 2;
		bo = next_bo;

		// if (i % 120 == 0) {
		// 	printf(
		// 		" drawtime = %08ld\n swaptime = %08ld\n locktime = %08ld\n fliptime = %08ld\n reletime = %08ld\n",
		// 		drawtime, swaptime, locktime, fliptime, releasetime
		// 	);
		// }
	}
	printf("before deinit_gl(), fds:\n");
	print_fds();

	ret = deinit_gl();
	if(ret) {
		printf("failed to deinit GL: %d\n", ret);
		return ret;
	}

	printf("before deinit_gbm(), fds:\n");
	print_fds();
	deinit_gbm();

	printf("After deinit_drm(), fds:\n");
	print_fds();
	ret = deinit_drm();
	if(ret) {
		printf("failed to deinit drm: %d\n", ret);
		return ret;
	}

	return ret;
}
