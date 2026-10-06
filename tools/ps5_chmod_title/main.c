/* ps5_chmod_title: payload for the console's ELF loader. Sets mode 0777 on
 * every file and directory under /data/homebrew/<title id> (PPSA88360 unless built with
 * -DRCOMP_CHMOD_TITLE="\"PPSA88361\"") and nothing else.
 *
 * FTP servers such as zftpd write 0666 and have no SITE CHMOD; the system then
 * refuses to exec eboot.bin (processSpawn 0x80aa001a, errno 13). Send this
 * after each tools/ps5_ftp_kit.py push. Its stdout comes back on the loader
 * socket: "ps5_chmod_title: <dir> fixed=N failed=0".
 *
 * Build (ps5-payload-sdk): prospero-clang -std=c11 -O2 -Wall -Wextra -Werror
 *   -o ps5_chmod_title.elf main.c
 * On Windows the SDK's win/prospero-clang.cmd needs clang 18 on PATH and
 * -fvisibility-nodllstorageclass=default. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The folder to fix: -DRCOMP_CHMOD_TITLE=\"PPSA88361\" for another title (default: GTA IV, PPSA88360). */
#ifndef RCOMP_CHMOD_TITLE
#define RCOMP_CHMOD_TITLE "PPSA88360"
#endif
#define ROOT "/data/homebrew/" RCOMP_CHMOD_TITLE

static int fixed, failed;

static void fix(const char *path) {
    if (chmod(path, 0777) == 0) {
        fixed++;
    } else {
        failed++;
        printf("ps5_chmod_title: chmod failed: %s\n", path);
    }
}

static void walk(const char *path) {
    fix(path);
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk(child);
        else
            fix(child);
    }
    closedir(dir);
}

int main(void) {
    walk(ROOT);
    printf("ps5_chmod_title: %s fixed=%d failed=%d\n", ROOT, fixed, failed);
    return failed ? 1 : 0;
}
