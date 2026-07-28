#include "container.h"
#include <sys/stat.h>
#include <dirent.h>

static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    return mkdir(path, 0755);
}

/* Fallback chroot implementation if pivot_root is unavailable */
static int do_chroot(const char *rootfs) {
    if (chroot(rootfs) < 0) {
        log_warn("chroot('%s') failed (%s)", rootfs, strerror(errno));
        return -1;
    }
    if (chdir("/") < 0) {
        log_warn("chdir('/') failed (%s)", strerror(errno));
        return -1;
    }
    return 0;
}

/* Pivot root to new rootfs */
static int do_pivot_root(const char *new_root) {
    /* Bind mount new_root to itself to ensure it is a mount point */
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) < 0) {
        log_warn("Failed to bind mount rootfs '%s': %s", new_root, strerror(errno));
        return -1;
    }

    char old_root[512];
    snprintf(old_root, sizeof(old_root), "%s/.old_root", new_root);
    if (ensure_dir_exists(old_root) < 0) {
        log_warn("Failed to create temporary directory '%s': %s", old_root, strerror(errno));
        return -1;
    }

    /* Perform syscall pivot_root */
    if (syscall(SYS_pivot_root, new_root, old_root) < 0) {
        log_warn("pivot_root failed (%s), trying chroot fallback", strerror(errno));
        rmdir(old_root);
        return do_chroot(new_root);
    }

    if (chdir("/") < 0) {
        log_warn("chdir('/') after pivot_root failed: %s", strerror(errno));
        return -1;
    }

    /* Unmount and remove old root */
    if (umount2("/.old_root", MNT_DETACH) < 0) {
        log_warn("Failed to unmount old root: %s", strerror(errno));
    }
    rmdir("/.old_root");

    return 0;
}

int setup_container_filesystem(const container_opts_t *opts) {
    /* 1. Make all mounts in child namespace private so mounts don't leak to host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        if (errno == EPERM || errno == EACCES) {
            log_warn("Host kernel restricted unprivileged mount namespace (AppArmor / userns restriction)");
        } else {
            log_warn("Failed to make root mount private: %s", strerror(errno));
        }
    }

    /* 2. Switch root filesystem if custom rootfs is specified */
    if (opts->rootfs && strlen(opts->rootfs) > 0) {
        if (opts->verbose) {
            log_info("[Child] Switching root filesystem to '%s'", opts->rootfs);
        }
        do_pivot_root(opts->rootfs);
    }

    /* 3. Mount /proc for container process visibility */
    ensure_dir_exists("/proc");
    umount2("/proc", MNT_DETACH);

    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
        if (errno == EPERM || errno == EACCES) {
            log_warn("Host kernel restricted unprivileged procfs mount");
        } else if (opts->verbose) {
            log_warn("Mounting /proc failed: %s", strerror(errno));
        }
    } else if (opts->verbose) {
        log_info("[Child] Mounted fresh /proc filesystem");
    }

    /* 4. Mount /sys read-only */
    ensure_dir_exists("/sys");
    umount2("/sys", MNT_DETACH);
    if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, NULL) < 0) {
        if (opts->verbose) {
            log_warn("Mounting /sys failed: %s", strerror(errno));
        }
    }

    /* 5. Mount /tmp as tmpfs */
    ensure_dir_exists("/tmp");
    if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=64M") < 0) {
        if (opts->verbose) {
            log_warn("Mounting /tmp failed: %s", strerror(errno));
        }
    }

    return 0;
}
