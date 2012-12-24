#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <time.h>
#include <libudev.h>
#include <pthread.h>
#include <linux/input.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>

#define EV_MAKE   1  /* when key pressed */
#define EV_BREAK  0  /* when key released */
#define EV_REPEAT 2  /* when key switches to repeating after short delay */
#define MAX_KBD_DEVICES 10
#define MAX_APM_BUF 256
#define WAIT_FOR_CLIENT 20 /* seconds to wait for the client to connect to the fifo */

static char *fifo_name = "/tmp/faps.fifo";
static unsigned int apm_interval = 10;
static unsigned int apm_fifo_interval = 1;

static int fifo = 0;
static unsigned long apm_buf[MAX_APM_BUF] = { 0 };
static unsigned int apm_idx = 0;
static struct timeval tv0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static const char** detect_kbd_devices()
{
  const char **devnodes = calloc(MAX_KBD_DEVICES, sizeof(char*));
  struct udev *udev;
  struct udev_enumerate *enumerate;
  struct udev_list_entry *devices, *dev_list_entry;
  struct udev_device *dev;
  int i = 0;

  udev = udev_new();
  if (!udev) {
    fprintf(stderr, "faps-d: Can't create udev.\n");
    exit(EXIT_FAILURE);
  }
  enumerate = udev_enumerate_new(udev);
  udev_enumerate_add_match_property(enumerate, "ID_INPUT_KEYBOARD", "1");
  udev_enumerate_scan_devices(enumerate);
  devices = udev_enumerate_get_list_entry(enumerate);
  udev_list_entry_foreach(dev_list_entry, devices) {
    const char *path, *devnode;
    path = udev_list_entry_get_name(dev_list_entry);
    dev = udev_device_new_from_syspath(udev, path);
    devnode = udev_device_get_devnode(dev);
    if (devnode) {
      devnodes[i] = strdup(devnode);
      i++;
    }
    udev_device_unref(dev);
  }
  udev_enumerate_unref(enumerate);
  udev_unref(udev);
  return devnodes;
}

static unsigned long get_msec()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  tv.tv_sec -= tv0.tv_sec;
  tv.tv_usec -= tv0.tv_usec;
  if (tv.tv_usec < 0) {
    --tv.tv_sec;
    tv.tv_usec += 1000000;
  }
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static unsigned int compute_apm()
{
  int i, idx;
  unsigned long now, delta;
  double apm;
  now = get_msec();
  apm = 0;
  delta = 0;

  pthread_mutex_lock(&mtx);
  for (i=0;i<MAX_APM_BUF;i++) {
    idx = apm_idx - i - 1;
    if (idx < 0) idx = MAX_APM_BUF - 1;
    if (!apm_buf[idx]) break;
    delta = now - apm_buf[idx];
    if (delta > apm_interval * 1000)
      break;
    apm += ((double)apm_interval * 1000.0 - (double)delta) / ((double)apm_interval * 500.0);
  }
  pthread_mutex_unlock(&mtx);

  return (unsigned long)(apm * 60.0 / (double)apm_interval);
}

static void* apm_monitor(void *arg)
{
  int *retval = calloc(1, sizeof(int));
  int wr;
  char msg[9];
  while (sleep(apm_fifo_interval) != -1) {
    snprintf(msg, 9, "APM:%d", compute_apm());
    wr = write(fifo, msg, sizeof(msg));
    if (wr <= 0) {
      if (wr == -1) {
        fprintf(stderr, "faps-d: Fifo write error: %s. Exiting apm monitor...\n", strerror(errno));
        *retval = 1;
      }
      pthread_exit(retval);
    }
  }
  fprintf(stderr, "faps-d: sleep failed with error: %s. Exiting apm monitor...\n", strerror(errno));
  *retval = 1;
  pthread_exit(retval);
}

static void* listen_kbd_dev(void *arg)
{
  char *device = (char*)arg;
  int *retval = calloc(1, sizeof(int));
  int input_fd = open(device, O_RDONLY);
  if (input_fd == -1) {
    fprintf(stderr, "faps-d: %s: Error opening device. Closing listener...\n", device);
    *retval = 1;
    pthread_exit(retval);
  }
  printf("faps-d: Listening on %s.\n", device);

  struct input_event event;
  bool ctrl_in_effect = false;
  char *msg;
  int wr;

  while (read(input_fd, &event, sizeof(struct input_event)) > 0) {
    if (event.type != EV_KEY) continue;
    /*printf("faps: %s: [%x]\n", device, event.code);*/
    if (event.value == EV_MAKE) { /* key pressed */
      unsigned long msec = get_msec();

      pthread_mutex_lock(&mtx);
      apm_buf[apm_idx] = msec;
      apm_idx++;
      if (apm_idx >= MAX_APM_BUF)
        apm_idx = 0;
      pthread_mutex_unlock(&mtx);

      if (event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL)
        ctrl_in_effect = true;
      else if (ctrl_in_effect) {
        switch (event.code) {
          case KEY_F9: msg = "F9"; break;
          case KEY_F10: msg = "F10"; break;
          case KEY_F11: msg = "F11"; break;
          case KEY_F12: msg = "F12"; break;
          default: msg = NULL;
        }
        if (msg) {
          wr = write(fifo, msg, sizeof(msg));
          if (wr <= 0) {
            if (wr == -1) {
              fprintf(stderr, "faps-d: %s: Fifo write error: %s. Closing listener...\n", device, strerror(errno));
              *retval = 1;
            }
            break;
          }
        }
      }
    }
    else if (event.value == EV_BREAK) { /* key released */
      if (event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL)
        ctrl_in_effect = false;
    }
  }
  close(input_fd);
  pthread_exit(retval);
}

static void on_sigpipe(int s)
{
  exit(EXIT_SUCCESS);
}

static void clean_fifo()
{
  if (fifo > 0) close(fifo);
  unlink(fifo_name);
}

void process_cmdline_args(int argc, char **argv)
{
  int c;
  opterr = 0;
  while ((c = getopt(argc, argv, "t:s:i:v:")) != -1)
    switch (c) {
      case 't':
        fifo_name = optarg;
        break;
      case 'i':
        apm_interval = atoi(optarg);
        break;
      case 'v':
        apm_fifo_interval = atoi(optarg);
        break;
      case '?':
        if (optopt == 't' || optopt == 's' || optopt == 'i' || optopt == 'v')
          fprintf (stderr, "faps-d: Option -%c requires an argument. Exiting...\n", optopt);
        else if (isprint(optopt))
          fprintf (stderr, "faps-d: Unknown option '-%c'. Exiting...\n", optopt);
        else
          fprintf (stderr, "faps-d: Unknown option character '\\x%x'. Exiting...\n", optopt);
        exit(EXIT_FAILURE);
      default:
        exit(EXIT_FAILURE);
    }

}

int main(int argc, char **argv)
{
  int i;
  gettimeofday(&tv0, 0);
  process_cmdline_args(argc, argv);

  /* create fifo and connect with client */

  if (mkfifo(fifo_name, 0666) == -1) {
    fprintf(stderr, "faps-d: Failed to create fifo %s with error: %s. Exiting...\n", fifo_name, strerror(errno));
    exit(EXIT_FAILURE);
  }
  atexit(clean_fifo);
  signal(SIGPIPE, on_sigpipe);
  for (i=0;i<WAIT_FOR_CLIENT;i++) {
    fifo = open(fifo_name, O_WRONLY | O_NONBLOCK);
    if (fifo != -1 || (fifo == -1 && errno != ENXIO)) break;
    sleep(1);
  }
  if (fifo == -1) {
    if (errno == ENXIO)
      fprintf(stderr, "faps-d: Client did not connect to the fifo %s in a timely manner. Not an OpenGL app? Exiting...\n", fifo_name);
    else
      fprintf(stderr, "faps-d: Failed to open fifo %s with error: %s. Exiting...\n", fifo_name, strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* create /dev/input/eventX listener pthreads */

  const char **devices = detect_kbd_devices();
  pthread_t kbd_tids[MAX_KBD_DEVICES] = { 0 };

  for (i=0;i<MAX_KBD_DEVICES;i++) {
    const char *device = devices[i];
    if (!device) break;
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, &listen_kbd_dev, (void*)device);
    if (ret != 0) {
      fprintf(stderr, "faps-d: %s: pthread_create failed with status %i.", device, ret);
      exit(EXIT_FAILURE);
    }
    kbd_tids[i] = tid;
  }

  /* create apm monitor thread */

  pthread_t apm_tid;
  int ret = pthread_create(&apm_tid, NULL, &apm_monitor, NULL);
  if (ret != 0) {
    fprintf(stderr, "faps-d: apm: pthread_create failed with status %i.", ret);
    exit(EXIT_FAILURE);
  }

  /* wait & exit program in case all listener treads close by themselves */

  for (i=0;i<MAX_KBD_DEVICES;i++) {
    pthread_t tid = kbd_tids[i];
    if (!tid) break;
    int ret = pthread_join(tid, NULL);
    printf("faps-d: %s: Listener closed with status %i.\n", devices[i], ret);
  }
  printf("faps-d: All listeners closed. Exiting...\n");
  return 0;       
}
