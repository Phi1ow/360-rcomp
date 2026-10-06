/* rcomp_chmod: ELF-loader payload for R-comp Installer.
 *
 * The console's FTP server writes files with mode 0666 and has no SITE CHMOD, so the system refuses to
 * exec eboot.bin (processSpawn 0x80aa001a, errno 13). A payload sent to the ELF loader runs un-sandboxed,
 * so its chmod reaches the real /data.
 *
 * The installer writes the title id ("PPSA" + 5 digits) to /data/rcomp-installer/chmod.request over FTP,
 * then sends this payload. It sets mode 0777 on /data/homebrew/<id> and everything under it, nothing
 * else, removes the request and prints one line on the loader socket:
 *   rcomp_chmod: /data/homebrew/<id> fixed=N failed=M
 * Built once with the ps5-payload SDK (prospero-clang); no title id is compiled in.
 *
 * It also records the capacity of /data for the title: a sandboxed title cannot query it (statvfs and
 * fstatvfs answer ENOSYS there, statfs is not exported to applications), yet games read the free bytes of
 * their storage device before saving. /data/homebrew/<id>/savedata/.rcomp-capacity receives
 *   total=<bytes>\nfree=<bytes>\nmeasured=<unix time>\n
 * measured now with statfs(2); R-comp's runtime reports it as the hard drive's size when it cannot measure. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REQUEST "/data/rcomp-installer/chmod.request"

static int fixed, failed;

static void fix(const char *path) {
    if (chmod(path, 0777) == 0) {
        fixed++;
    } else {
        failed++;
        printf("rcomp_chmod: chmod failed: %s\n", path);
    }
}

static void walk(const char *path, int depth) {
    fix(path);
    if (depth > 64)
        return;
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char child[1024];
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) {
            failed++;
            printf("rcomp_chmod: path too long under %s\n", path);
            continue;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            failed++;
            printf("rcomp_chmod: stat failed: %s\n", child);
            continue;
        }
        if (S_ISDIR(st.st_mode))
            walk(child, depth + 1);
        else if (S_ISREG(st.st_mode))
            fix(child);
    }
    closedir(dir);
}

static int valid_title_id(const char *id) {
    if (strncmp(id, "PPSA", 4) != 0 || strlen(id) != 9)
        return 0;
    for (int i = 4; i < 9; i++)
        if (id[i] < '0' || id[i] > '9')
            return 0;
    return 1;
}

int main(void) {
    char id[32] = {0};
    FILE *f = fopen(REQUEST, "r");
    if (!f) {
        printf("rcomp_chmod: no request at %s\n", REQUEST);
        return 2;
    }
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    unlink(REQUEST);
    while (n > 0 && (id[n - 1] == '\n' || id[n - 1] == '\r' || id[n - 1] == ' '))
        id[--n] = 0;
    if (!valid_title_id(id)) {
        printf("rcomp_chmod: invalid title id in request\n");
        return 2;
    }
    char root[64];
    snprintf(root, sizeof root, "/data/homebrew/%s", id);
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("rcomp_chmod: %s fixed=0 failed=1 (missing)\n", root);
        return 1;
    }
    // Capacity of /data for the title (written before the walk, so the walk also opens its mode).
    char saves[96], capacity[128];
    snprintf(saves, sizeof saves, "%s/savedata", root);
    snprintf(capacity, sizeof capacity, "%s/.rcomp-capacity", saves);
    mkdir(saves, 0777);
    struct statfs fs;
    if (statfs("/data", &fs) == 0) {
        FILE* out = fopen(capacity, "w");
        if (out) {
            const unsigned long long total = (unsigned long long)fs.f_blocks * fs.f_bsize;
            const unsigned long long free_bytes = fs.f_bavail > 0 ? (unsigned long long)fs.f_bavail * fs.f_bsize : 0;
            fprintf(out, "total=%llu\nfree=%llu\nmeasured=%lld\n", total, free_bytes, (long long)time(NULL));
            fclose(out);
            printf("rcomp_chmod: capacity total=%llu free=%llu\n", total, free_bytes);
        } else {
            printf("rcomp_chmod: capacity file not written: %s\n", capacity);
        }
    } else {
        printf("rcomp_chmod: statfs(/data) failed\n");
    }
    walk(root, 0);
    printf("rcomp_chmod: %s fixed=%d failed=%d\n", root, fixed, failed);
    return failed ? 1 : 0;
}
