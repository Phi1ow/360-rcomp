/*
 * R-comp - process shims for PS5_Vulkan (fork @17350536) linked into a title.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gpu/vulkan/ps5/build-ps5.sh (contract ps5vk-driver) copies the driver
 * archives and renames these references in the copies only (llvm-objcopy
 * --redefine-sym). The redirection points are the ones of the console-proven
 * recipe in Phi1ow/Emu-3 eden/ps5/scripts/Build-VulkanApiProbe.ps1 (Super
 * Mario Galaxy ran on the fork through it); the code below is written for
 * this project.
 *
 *   getenv/setenv/unsetenv -> rcomp_vk_*   process-local environment; the
 *                                          driver switch PS5VK_EXPERIMENTAL
 *                                          is fixed once by the title
 *   sceAgcInit(version)    -> rcomp_vk_agc_init: the driver uses the payload
 *                             SDK's one-argument form, the console library
 *                             takes (state*, revision)
 *   pthread_attr_setstack  -> ENOTSUP (the driver then falls back to
 *                             pthread_attr_setstacksize)
 *   pthread_mutex_timedlock-> trylock + sceKernelUsleep polling
 *   mkstemp/system/getrlimit, usleep -> explicit implementations or ENOSYS
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

int sceKernelUsleep(unsigned int microseconds);
int sceAgcInit(void *state, unsigned int revision);

static pthread_mutex_t env_mu = PTHREAD_MUTEX_INITIALIZER;
struct env_entry {
   struct env_entry *next;
   char *name, *value;
};
static struct env_entry *env_head;
static int vk_profile = -1; /* -1 unset, 0 default, 1 experimental */
static char profile_0[] = "0", profile_1[] = "1";

void
rcomp_vk_set_profile(int experimental)
{
   pthread_mutex_lock(&env_mu);
   if (vk_profile < 0)
      vk_profile = experimental ? 1 : 0;
   pthread_mutex_unlock(&env_mu);
}

static int
valid_name(const char *n)
{
   return n && *n && !strchr(n, '=');
}

static struct env_entry *
find(const char *name)
{
   for (struct env_entry *e = env_head; e; e = e->next)
      if (!strcmp(e->name, name))
         return e;
   return NULL;
}

char *
rcomp_vk_getenv(const char *name)
{
   if (!valid_name(name))
      return NULL;
   pthread_mutex_lock(&env_mu);
   char *v;
   if (!strcmp(name, "PS5VK_EXPERIMENTAL")) {
      if (vk_profile < 0)
         vk_profile = 0; /* first read freezes the default profile */
      v = vk_profile ? profile_1 : profile_0;
   } else {
      struct env_entry *e = find(name);
      v = e ? e->value : NULL;
   }
   pthread_mutex_unlock(&env_mu);
   return v;
}

int
rcomp_vk_setenv(const char *name, const char *value, int overwrite)
{
   if (!valid_name(name) || !value) {
      errno = EINVAL;
      return -1;
   }
   pthread_mutex_lock(&env_mu);
   struct env_entry *e = find(name);
   if (e && e->value && !overwrite) {
      pthread_mutex_unlock(&env_mu);
      return 0;
   }
   char *copy = strdup(value);
   if (!copy || (!e && !(e = calloc(1, sizeof *e))) || (!e->name && !(e->name = strdup(name)))) {
      pthread_mutex_unlock(&env_mu);
      errno = ENOMEM;
      return -1;
   }
   if (!find(name)) {
      e->next = env_head;
      env_head = e;
   }
   e->value = copy; /* old values stay alive: callers may cache getenv pointers */
   pthread_mutex_unlock(&env_mu);
   return 0;
}

int
rcomp_vk_unsetenv(const char *name)
{
   if (!valid_name(name)) {
      errno = EINVAL;
      return -1;
   }
   pthread_mutex_lock(&env_mu);
   struct env_entry *e = find(name);
   if (e)
      e->value = NULL;
   pthread_mutex_unlock(&env_mu);
   return 0;
}

int
rcomp_vk_agc_init(unsigned int revision)
{
   static uint64_t state; /* kept by AGC for the life of the process */
   return sceAgcInit(&state, revision);
}

int
rcomp_vk_pthread_attr_setstack(pthread_attr_t *attr, void *stack, size_t size)
{
   (void)attr;
   (void)stack;
   (void)size;
   return ENOTSUP;
}

int
rcomp_vk_usleep(unsigned int us)
{
   int rc = sceKernelUsleep(us);
   if (rc == 0)
      return 0;
   errno = ((unsigned)rc & 0xffff0000u) == 0x80020000u ? (int)((unsigned)rc & 0xffffu) : EINVAL;
   return -1;
}

int
rcomp_vk_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until)
{
   if (!m)
      return EINVAL;
   int rc = pthread_mutex_trylock(m);
   if (rc != EBUSY)
      return rc;
   if (!until || until->tv_nsec < 0 || until->tv_nsec >= 1000000000)
      return EINVAL;
   for (;;) {
      struct timespec now;
      if (clock_gettime(CLOCK_REALTIME, &now) != 0)
         return errno;
      if (now.tv_sec > until->tv_sec || (now.tv_sec == until->tv_sec && now.tv_nsec >= until->tv_nsec))
         return ETIMEDOUT;
      sceKernelUsleep(100);
      rc = pthread_mutex_trylock(m);
      if (rc != EBUSY)
         return rc;
   }
}

int
rcomp_vk_mkstemp(char *pattern)
{
   size_t n = pattern ? strlen(pattern) : 0;
   if (n < 6 || memcmp(pattern + n - 6, "XXXXXX", 6)) {
      errno = EINVAL;
      return -1;
   }
   static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
   static unsigned counter;
   for (unsigned attempt = 0; attempt < 256; attempt++) {
      unsigned v = (unsigned)getpid() * 2654435761u ^ (++counter * 40503u) ^ (unsigned)time(NULL);
      for (int i = 0; i < 6; i++, v /= 36)
         pattern[n - 6 + i] = alphabet[v % 36];
      int fd = open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
      if (fd >= 0 || errno != EEXIST)
         return fd;
   }
   errno = EEXIST;
   return -1;
}

int
rcomp_vk_system(const char *command)
{
   if (!command)
      return 0; /* no command processor */
   errno = ENOSYS;
   return -1;
}

int
rcomp_vk_getrlimit(int resource, struct rlimit *rl)
{
   (void)resource;
   (void)rl;
   errno = ENOSYS;
   return -1;
}
