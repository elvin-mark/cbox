/* Step 5: Switching root filesystems with pivot_root (demonstration). */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: sudo %s <rootfs-dir>\n", argv[0]);
        return 0;
    }

    const char *new_root = argv[1];
    if (unshare(CLONE_NEWNS) < 0) {
        perror("unshare");
        return 1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("MS_PRIVATE");
        return 1;
    }
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) < 0) {
        perror("bind new_root");
        return 1;
    }

    char old_root[512];
    snprintf(old_root, sizeof(old_root), "%s/.old_root", new_root);
    mkdir(old_root, 0755);

    if (syscall(SYS_pivot_root, new_root, old_root) < 0) {
        perror("pivot_root");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }
    umount2("/.old_root", MNT_DETACH);
    rmdir("/.old_root");
    printf("Switched root to %s!\n", new_root);
    return 0;
}
