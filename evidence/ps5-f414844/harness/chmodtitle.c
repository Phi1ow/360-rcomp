/* chmodtitle: sets mode 0777 on every file and directory under
 * /data/homebrew/PPSA88360 (and nothing else), like the FTP-less "chmod
 * repair" step of the Eden deploy. zftpd uploads files as 0666, and
 * SceSysCore then refuses to exec eboot.bin (errno 13). */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define ROOT "/data/homebrew/PPSA88360"

static int fixed, failed;

static void walk(const char *path) {
    if (chmod(path, 0777) == 0)
        fixed++;
    else {
        failed++;
        printf("chmodtitle: chmod failed: %s\n", path);
    }
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
        else if (chmod(child, 0777) == 0)
            fixed++;
        else {
            failed++;
            printf("chmodtitle: chmod failed: %s\n", child);
        }
    }
    closedir(dir);
}

int main(void) {
    walk(ROOT);
    printf("chmodtitle: %s fixed=%d failed=%d\n", ROOT, fixed, failed);
    return failed ? 1 : 0;
}
