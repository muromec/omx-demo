#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>

static int fb0, fb1, dc0, dc1;

int ioctl(int filedes, int request, void *arg) {
  int (*__real_ioctl)(int, int, void*)=dlsym(RTLD_NEXT, "ioctl");

  if (filedes == fb0) filedes=fb1;
  if (filedes == dc0) filedes=dc1;

  return __real_ioctl(filedes, request, arg);
}


int open(char* fname, int flags) {
    int (*__real_open)(char *, int)=dlsym(RTLD_NEXT, "open");

    int ret = __real_open(fname, flags);

    if(!strcmp(fname, "/dev/fb0")) fb0 = ret;
    if(!strcmp(fname, "/dev/fb1")) fb1 = ret;

    if(!strcmp(fname, "/dev/tegra_dc_0")) dc0 = ret;
    if(!strcmp(fname, "/dev/tegra_dc_1")) dc1 = ret;

    return ret;

}
