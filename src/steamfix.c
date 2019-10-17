#define _GNU_SOURCE

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Work around timeout on login issue */

#include <sys/epoll.h>

static int (*libc_epoll_ctl)(int, int, int, struct epoll_event*) = NULL;

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {

  if (!libc_epoll_ctl) {
    libc_epoll_ctl = dlsym(RTLD_NEXT, "epoll_ctl");
  }

  if (event) {
    event->events |= EPOLLERR;
  }

  int err = libc_epoll_ctl(epfd, op, fd, event);

  if (err == -1 && op == EPOLL_CTL_MOD && errno == ENOENT) {
    errno = 0;
    err   = libc_epoll_ctl(epfd, EPOLL_CTL_ADD, fd, event);
  }

  if (err == -1) {
    perror(__func__);
  }

  return err;
}

/* Silence obnoxious "libudev: udev_monitor_new_from_netlink_fd: error getting socket: Address family not supported by protocol" warning */

void* udev_monitor_new_from_netlink(void* udev, const char* name) {
  return NULL;
}

void* udev_monitor_new_from_netlink_fd(void* udev, const char* name, int fd) {
  assert(0);
}

/* Do not allow Breakpad to override libSegFault handlers */

#include <execinfo.h>
#include <signal.h>

static int (*libc_sigaction)(int, const struct sigaction*, struct sigaction*) = NULL;

int sigaction(int sig, const struct sigaction* restrict act, struct sigaction* restrict oact) {

  if (!libc_sigaction) {
    libc_sigaction = dlsym(RTLD_NEXT, "sigaction");
  }

  void* buffer[2];
  int nframes = backtrace(buffer, 2);
  assert(nframes == 2);

  char* caller_str = *backtrace_symbols(buffer + 1, 1);
  assert(caller_str != NULL);

  char* p = strrchr(caller_str, '/');
  if (p && strncmp("crashhandler.so", p + 1, sizeof("crashhandler.so") - 1) == 0) {
    return EINVAL;
  }

  return libc_sigaction(sig, act, oact);
}

/* Let's rewrite a few commands as well... */

static int (*libc_system)(const char*) = NULL;

#define XDG_OPEN_CMD "LD_LIBRARY_PATH=\"$SYSTEM_LD_LIBRARY_PATH\" PATH=\"$SYSTEM_PATH\" '/usr/bin/xdg-open' "

int system(const char* command) {

  if (!libc_system) {
    libc_system = dlsym(RTLD_NEXT, "system");
  }

  /* such as xdg-open */

  if (strncmp(command, XDG_OPEN_CMD, sizeof(XDG_OPEN_CMD) - 1) == 0) {

    const char* xdg_open_args = &command[sizeof(XDG_OPEN_CMD) - 1];

    char* format_str = "PATH=/usr/bin:/usr/local/bin LD_LIBRARY_PATH='' LD_PRELOAD='' xdg-open %s";

    int   buf_len = strlen(format_str) + strlen(xdg_open_args) + 1;
    char* buf     = malloc(buf_len);

    snprintf(buf, buf_len, format_str, xdg_open_args);

    int err = libc_system(buf);
    free(buf);

    return err;
  }

  /* or steamwebhelper, which currently doesn't work all that well, so it's disabled by default */

  if (strstr(command, "steamwebhelper.sh")) {

    char* browser_enabled = getenv("STEAM_BROWSER");
    if (browser_enabled && strcmp(browser_enabled, "1") == 0) {

      char* format_str =
        "LD_PRELOAD=webfix.so"
        " LD_LIBRARY_PATH=/compat/linux/usr/lib64/nss:${LD_LIBRARY_PATH}"
        " '%s.patched' %s"
        " --no-sandbox"
        " --no-zygote"
        //" --enable-logging=stderr"
        //" --v=0"
      ;

      int   buf_len = strlen(format_str) + strlen(command) + 1;
      char* buf     = malloc(buf_len);

      char* webhelper_path = strdup(command + 1);
      char* webhelper_args = strstr(webhelper_path, "steamwebhelper.sh' ") + sizeof("steamwebhelper.sh' ") - 1;
      assert(webhelper_args[-2] == '\'');
      webhelper_args[-2] = '\0';

      snprintf(buf, buf_len, format_str, webhelper_path, webhelper_args);

      free(webhelper_path);

      fprintf(stderr, "[[%s]]\n", buf);

      int err = libc_system(buf);
      free(buf);

      return err;

    } else {
      return 1;
    }
  }

  return libc_system(command);
}

/* Lie to Steam about /home being a directory. The Steam client skips symlinks in Add Library Folder / Add a (non-Steam) Gamed dialogs and,
  if it was started from /home/... path, it will get quite confused without this workaround. */

#include <sys/stat.h>

static int (*libc_lxstat64)(int, const char*, struct stat64*) = NULL;

int __lxstat64(int ver, const char* path, struct stat64* stat_buf) {

  if (!libc_lxstat64) {
    libc_lxstat64 = dlsym(RTLD_NEXT, "__lxstat64");
  }

  if (strcmp(path, "//home") == 0) {
    return libc_lxstat64(ver, "/usr/home", stat_buf);
  } else {
    return libc_lxstat64(ver, path, stat_buf);
  }
}

/* Also trick Linuxulator into listing actual /usr content instead of /compat/linux/usr */

#include <dirent.h>

static int (*libc_scandir64)(const char*, struct dirent64***,
  int (*)(const struct dirent64*), int (*)(const struct dirent64**, const struct dirent64**)) = NULL;

int scandir64(const char* dir, struct dirent64*** namelist,
  int (*sel)(const struct dirent64*), int (*cmp)(const struct dirent64**, const struct dirent64**))
{
  if (!libc_scandir64) {
    libc_scandir64 = dlsym(RTLD_NEXT, "scandir64");
  }

  if (strcmp(dir, "/usr") == 0) {
    return libc_scandir64("/usr/local/..", namelist, sel, cmp);
  } else {
    return libc_scandir64(dir, namelist, sel, cmp);
  }
}

/* Correct uname because for whatever reason Linuxulator reports "i686" to 32-bit applications on amd64 */

#include <unistd.h>
#include <sys/utsname.h>

static int (*libc_uname)(struct utsname*) = NULL;

int uname(struct utsname* name) {

  if (!libc_uname) {
    libc_uname = dlsym(RTLD_NEXT, "uname");
  }

  int err = libc_uname(name);
  if (err == 0 && access("/usr/lib64", F_OK) == 0) {
    snprintf(name->machine, _UTSNAME_MACHINE_LENGTH, "x86_64");
  }

  return err;
}

/* Handle Steam restart */

#include <stdbool.h>
#include <unistd.h>

static int    program_argc;
static char** program_argv;
static char   program_path[PATH_MAX];

__attribute__((constructor))
static void restart_fix_init(int argc, char** argv, char** env) {

  program_argc = argc;
  program_argv = argv;

  ssize_t nchars = readlink("/proc/self/exe", program_path, sizeof(program_path));
  assert(nchars > 0);
  program_path[nchars] = '\0';
}

extern char** environ;

static bool drop_urls_on_restart = false;

static __attribute__((__noreturn__)) void restart() {

  printf("Restarting Steam...\n");

  char pidfile_path[PATH_MAX];
  snprintf(pidfile_path, sizeof(pidfile_path), "%s/%s", getenv("HOME"), ".steam/steam.pid");

  unlink(pidfile_path);

  if (drop_urls_on_restart) {

    char** argv = malloc(sizeof(char*) * (program_argc + 1));

    int j = 0;
    for (int i = 0; i < program_argc; i++) {
      char* arg = program_argv[i];
      if (strncmp(arg, "steam://", sizeof("steam://") - 1) != 0) {
        argv[j] = arg; j++;
      }
    }
    argv[j] = NULL;

    execve(program_path, argv, environ);

  } else {
    execve(program_path, program_argv, environ);
  }

  perror("execve");
  abort();
}

static __attribute__((__noreturn__)) void (*libc_exit)(int) = NULL;

void exit(int status) {

  if (status == 42) {

    system("patch-steam");
    restart();

  } else {

    if (!libc_exit) {
      libc_exit = dlsym(RTLD_NEXT, "exit");
    }

    libc_exit(status);
  }
}

static int (*libc_fputs)(const char*, FILE*) = NULL;

int fputs(const char* str, FILE* stream) {

  if (!libc_fputs) {
    libc_fputs = dlsym(RTLD_NEXT, "fputs");
  }

  if (stream == stderr && strncmp(str, "ExecuteSteamURL:", sizeof("ExecuteSteamURL:") - 1) == 0) {
    drop_urls_on_restart = true;
  }

  return libc_fputs(str, stream);
}

/* We need to redirect those futex syscalls */

static void* (*libc_dlmopen)(Lmid_t, const char*, int) = NULL;

void* dlmopen(Lmid_t lmid, const char* path, int mode) {

  if (!libc_dlmopen) {
    libc_dlmopen = dlsym(RTLD_NEXT, "dlmopen");
  }

  void* p = NULL;

  if (strstr(path, "chromehtml.so") != NULL) {

    char* format_str = "%s.patched";

    int   buf_len = strlen(format_str) + strlen(path) + 1;
    char* buf     = malloc(buf_len);

    snprintf(buf, buf_len, format_str, path);

    p = libc_dlmopen(lmid, buf, mode);

    free(buf);

  } else {
    p = libc_dlmopen(lmid, path, mode);
  }

  return p;
}
