// The pinned SDK's statvfs emulation reports fixed 64/16 GiB capacities. These
// are not observations of the mounted volume. R-comp reports unavailable
// metadata until this title can query the native filesystem accurately.
#include <errno.h>
#include <sys/statvfs.h>
int rcomp_radv_statvfs(const char* path,struct statvfs* output) {
    (void)path;(void)output;errno=ENOSYS;return -1;
}
int rcomp_radv_fstatvfs(int fd,struct statvfs* output) {
    (void)fd;(void)output;errno=ENOSYS;return -1;
}
