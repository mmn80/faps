/*
faps - fps & apm monitoring and screen capture for Linux OpenGL programs.
Copyright (c) 2012 by Călin Ardelean <calinucs@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#ifndef __USE_GNU
/* otherwise RTLD_NEXT won't be defined in recent glibc versions */
#define __USE_GNU
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>

#define MAX_BUF 1024

static unsigned long print_interval; /* msec */
static unsigned long frame_interval; /* usec */
static float fps_pos_x, fps_pos_y;
static char* fifo_path = "/tmp/faps.fifo";

static bool capture_pic, capture_vid, show_fps, show_apm;

static int cur_fps = 0;
static int cur_apm = 0;
static pthread_t key_tid;
static int fifo = 0;

static int init(void);
static void overlay(void);
static unsigned long get_usec(void);
static void* listen_fifo(void* arg);
static void close_fifo();

static void (*swap_buffers)(Display*, GLXDrawable);
static void (*glUseProgramObjectARB)(unsigned int);
static unsigned int (*glGetHandleARB)(unsigned int);


void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
  unsigned long usec, interv;
  static unsigned long prev_frame, prev_print;
  static unsigned long frames;

  if (!swap_buffers && init() == -1)
    return;

  /*if(opt->capture_shot || opt->capture_vid) {
    char fname[64];
    int xsz, ysz;
    static int img_xsz = -1, img_ysz = -1;
    static uint32_t *img;

    int vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    xsz = vp[2];
    ysz = vp[3];

    if(opt->capture_shot) {
      printf("grabbing image %dx%d\n", xsz, ysz);
    }

    if(xsz != img_xsz || ysz != img_ysz) {
      free(img);
      if(!(img = malloc(xsz * ysz * sizeof *img))) {
        perror("frapix: failed to allocate memory");
      }
      img_xsz = xsz;
      img_ysz = ysz;
    }

    glReadPixels(0, 0, xsz, ysz, GL_RGBA, GL_UNSIGNED_BYTE, img);

    sprintf(fname, opt->shot_fname, cap_num++);

    //set_image_option(IMG_OPT_COMPRESS, 1);
    //set_image_option(IMG_OPT_INVERT, 1);
    //if(save_image(fname, img, xsz, ysz, IMG_FMT_TGA) == -1) {
    //fprintf(stderr, "frapix: failed to save image: %s\n", opt->shot_fname);
    //}
    if(img_save_pixels(fname, img, xsz, ysz, IMG_FMT_RGBA32) == -1) {
      fprintf(stderr, "frapix: failed to save image: %s\n", opt->shot_fname);
    }
    opt->capture_shot = 0;
  }*/

  if (show_fps || show_apm)
    overlay();

  swap_buffers(dpy, drawable);

  usec = get_usec();
  interv = usec - prev_print;

  if (interv >= print_interval * 1000 && interv > 0) {
    cur_fps = 1000000 * frames / interv;
    prev_print = usec;
    frames = 0;
  }
  else frames++;

  if (frame_interval > 0) {
    interv = usec - prev_frame;
    if (interv > 0 && interv < frame_interval) {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = (frame_interval - interv) * 1000;
      if (nanosleep(&ts, 0) == -1) {
        fprintf(stderr, "faps: nanosleep failed with error: %s.\n", strerror(errno));
        exit(EXIT_FAILURE);
      }
    }
    prev_frame = get_usec();
  }
}

static int init(void)
{
  char *env;

  if (!swap_buffers) {
    if (!(swap_buffers = dlsym(RTLD_NEXT, "glXSwapBuffers"))) {
      fprintf(stderr, "faps: Symbol glXSwapBuffers not found: %s.\n", dlerror());
      return -1;
    }
  }

  print_interval = 1000;
  frame_interval = 0;
  fps_pos_x = 0.95;
  fps_pos_y = 0.925;
  show_fps = true;
  show_apm = true;

  if ((env = getenv("FAPS_FPS_UPDATE_RATE"))) {
    if (!isdigit(*env)) {
      fprintf(stderr, "faps: FAPS_FPS_UPDATE_RATE must be set to the period of FPS update in milliseconds.\n");
      return -1;
    }
    else print_interval = atoi(env);
  }

  if ((env = getenv("FAPS_FPS_LIMIT"))) {
    printf("env: %s\n", env);
    if (!isdigit(*env)) {
      fprintf(stderr, "faps: FAPS_FPS_LIMIT must be followed by the maximum FPS number.\n");
      return -1;
    }
    else {
      int max_fps = atoi(env);
      if (max_fps) {
        frame_interval = 1000000 / max_fps;
        printf("faps: Enforcing FPS limit: %d FPS (interval: %ld usec)\n", max_fps, frame_interval);
      }
    }
  }

  if ((env = getenv("FAPS_FIFO_PATH")))
    fifo_path = strdup(env);

  fifo = open(fifo_path, O_RDONLY);
  if (fifo == -1) {
    fprintf(stderr, "faps: Failed to open fifo %s with error: %s. No key actions available.\n", fifo_path, strerror(errno));
    return -1;
  }
  atexit(close_fifo); 

  int ret = pthread_create(&key_tid, NULL, &listen_fifo, NULL);
  if (ret != 0) {
    fprintf(stderr, "faps: Creating thread for %s listener failed with error %i.", fifo_path, ret);
    return -1;
  }

  return 0;
}

static void* listen_fifo(void* arg)
{
  int rd;
  char buf[MAX_BUF];
  char apm_str[5];

  while ((rd = read(fifo, buf, MAX_BUF)) > 0) {
    /*printf("faps: Received [%s]\n", buf);*/
    if (strcmp(buf, "F9") == 0)
      show_fps = !show_fps;
    else if (strcmp(buf, "F10") == 0)
      show_apm = !show_apm;
    else if (strcmp(buf, "F11") == 0 && !capture_vid)
      capture_vid = true;
    else if (strcmp(buf, "F12") == 0 && !capture_pic)
      capture_pic = true;
    else if (strncmp(buf, "APM:", 4) == 0) {
      strncpy(apm_str, buf + 4, strlen(buf) - 3);
      cur_apm = atoi(apm_str);
    }
  }
  if (rd == -1)
    fprintf(stderr, "faps: Fifo %s read error: %s. Daemon ok? No key actions available from now on.\n", fifo_path, strerror(errno));
  else
    fprintf(stderr, "faps: Fifo %s EOF encountered. Daemon ok? No key actions available from now on.\n", fifo_path);
  close(fifo);
  fifo = 0;
  return NULL;
}

static void close_fifo()
{
  if (fifo > 0) close(fifo);
}






/* draw overlay functions */

static int count_digits(int x)
{
  int dig = 0;
  if (!x) return 1;

  while(x) {
    x /= 10;
    dig++;
  }
  return dig;
}

static int diglist[10];

#define XSZ		0.01
#define YSZ		0.025
#define DX		0.0025
#define DY              0.005

static struct {
  float x, y;
}
led[7][4] = {
  {{-XSZ - DX, 0}, {-XSZ + DX, 0}, {-XSZ + DX, YSZ + DY}, {-XSZ - DX, YSZ + DY}},
  {{-XSZ - DX, -YSZ - DY}, {-XSZ + DX, -YSZ - DY}, {-XSZ + DX, 0}, {-XSZ - DX, 0}},
  {{-XSZ - DX, -YSZ - DY}, {XSZ + DX, -YSZ - DY}, {XSZ + DX, -YSZ + DY}, {-XSZ - DX, -YSZ + DY}},
  {{XSZ - DX, -YSZ - DY}, {XSZ + DX, -YSZ - DY}, {XSZ + DX, 0}, {XSZ - DX, 0}},
  {{XSZ - DX, 0}, {XSZ + DX, 0}, {XSZ + DX, YSZ + DY}, {XSZ - DX, YSZ + DY}},
  {{-XSZ - DX, YSZ - DY}, {XSZ + DX, YSZ - DY}, {XSZ + DX, YSZ + DY}, {-XSZ - DX, YSZ + DY}},
  {{-XSZ - DX, -DY}, {XSZ + DX, -DY}, {XSZ + DX, +DY}, {-XSZ - DX, +DY}}
};

static int seglut[10][7] = {
  {1, 1, 1, 1, 1, 1, 0},  /* 0 */
  {0, 0, 0, 1, 1, 0, 0},  /* 1 */
  {0, 1, 1, 0, 1, 1, 1},  /* 2 */
  {0, 0, 1, 1, 1, 1, 1},  /* 3 */
  {1, 0, 0, 1, 1, 0, 1},  /* 4 */
  {1, 0, 1, 1, 0, 1, 1},  /* 5 */
  {1, 1, 1, 1, 0, 1, 1},  /* 6 */
  {0, 0, 0, 1, 1, 1, 0},  /* 7 */
  {1, 1, 1, 1, 1, 1, 1},  /* 8 */
  {1, 0, 1, 1, 1, 1, 1}   /* 9 */
};

static void draw_digit(int x)
{
  int i;
  if (x < 0 || x > 9) return;
  if (!diglist[x]) {
    diglist[x] = glGenLists(1);
    glNewList(diglist[x], GL_COMPILE);
    glBegin(GL_QUADS);
    for (i=0; i<7; i++) {
      if (seglut[x][i]) {
        glVertex2f(led[i][0].x, led[i][0].y);
        glVertex2f(led[i][1].x, led[i][1].y);
        glVertex2f(led[i][2].x, led[i][2].y);
        glVertex2f(led[i][3].x, led[i][3].y);
      }
    }
    glEnd();
    glEndList();
  }
  glCallList(diglist[x]);
}

static void overlay(void)
{
  int i, n, digits;
  unsigned int sdr = 0;

  glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glTranslatef(fps_pos_x, fps_pos_y, 0);

  glDisable(GL_LIGHTING);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_TEXTURE_1D);
  glDisable(GL_TEXTURE_2D);
  glColorMask(1, 1, 1, 1);

  if (glUseProgramObjectARB && glGetHandleARB) {
    sdr = glGetHandleARB(GL_PROGRAM_OBJECT_ARB);
    glUseProgramObjectARB(0);
  }

  if (show_fps) {
    n = cur_fps;
    digits = count_digits(n);
    glColor3f(1.0f, 1.0f, 0.0f);
    for (i=0; i<digits; i++) {
      draw_digit(n % 10);
      n /= 10;
      glTranslatef(-XSZ * 3.0, 0, 0);
    }
  }
  if (show_apm) {
    glTranslatef(XSZ * 3.0 * digits, -YSZ * 3.0, 0);
    n = cur_apm;
    digits = count_digits(n);
    glColor3f(0.0f, 1.0f, 0.0f);
    for (i=0; i<digits; i++) {
      draw_digit(n % 10);
      n /= 10;
      glTranslatef(-XSZ * 3.0, 0, 0);
    }
  }

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();

  if (glUseProgramObjectARB && sdr)
    glUseProgramObjectARB(sdr);

  glPopAttrib();
}

static unsigned long get_usec(void)
{
  struct timeval tv;
  static struct timeval tv0;

  gettimeofday(&tv, 0);

  if (tv0.tv_sec == 0 && tv0.tv_usec == 0) {
    tv0 = tv;
    return 0;
  }
  tv.tv_sec -= tv0.tv_sec;
  tv.tv_usec -= tv0.tv_usec;
  if (tv.tv_usec < 0) {
    --tv.tv_sec;
    tv.tv_usec += 1000000;
  }
  return tv.tv_sec * 1000000 + tv.tv_usec;
}
