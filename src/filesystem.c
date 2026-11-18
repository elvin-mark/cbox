#include "container.h"
#include <dirent.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return -1;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ensure_file_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return 0;
    }

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return -1;
    }

    char *dir = dirname(tmp);
    ensure_dir_exists(dir);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return -1;
}

static void join_rootfs_path(char *out, size_t out_len, const char *rootfs, const char *rel) {
    while (*rel == '/') {
        rel++;
    }
    if (*rel == '\0') {
        snprintf(out, out_len, "%s", rootfs);
    } else {
        snprintf(out, out_len, "%s/%s", rootfs, rel);
    }
}

/* Populate minimal /dev inside rootfs before pivot_root */
static void setup_dev_nodes(const char *rootfs, bool verbose) {
    char dev_dir[512];
    join_rootfs_path(dev_dir, sizeof(dev_dir), rootfs, "dev");
    ensure_dir_exists(dev_dir);

    if (mount("tmpfs", dev_dir, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=64k") < 0) {
        if (verbose) {
            log_warn("[Child] Failed to mount tmpfs on %s: %s", dev_dir, strerror(errno));
        }
    }

    char sub[560];
    snprintf(sub, sizeof(sub), "%s/pts", dev_dir);
    ensure_dir_exists(sub);
    mount("devpts", sub, "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620");

    snprintf(sub, sizeof(sub), "%s/shm", dev_dir);
    ensure_dir_exists(sub);
    mount("tmpfs", sub, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=64M");

    snprintf(sub, sizeof(sub), "%s/mqueue", dev_dir);
    ensure_dir_exists(sub);

    static const struct {
        const char *name;
        unsigned int major;
        unsigned int minor;
        mode_t mode;
    } devs[] = {
        {"null", 1, 3, S_IFCHR | 0666},
        {"zero", 1, 5, S_IFCHR | 0666},
        {"full", 1, 7, S_IFCHR | 0666},
        {"random", 1, 8, S_IFCHR | 0666},
        {"urandom", 1, 9, S_IFCHR | 0666},
        {"tty", 5, 0, S_IFCHR | 0666},
    };

    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        char node[560];
        snprintf(node, sizeof(node), "%s/%s", dev_dir, devs[i].name);
        if (mknod(node, devs[i].mode, makedev(devs[i].major, devs[i].minor)) < 0) {
            /* Unprivileged userns cannot mknod; bind-mount from host /dev instead */
            char host_dev[64];
            snprintf(host_dev, sizeof(host_dev), "/dev/%s", devs[i].name);
            if (ensure_file_exists(node) == 0) {
                mount(host_dev, node, NULL, MS_BIND, NULL);
            }
        }
    }

    static const struct {
        const char *link_name;
        const char *target;
    } symlinks[] = {
        {"fd", "/proc/self/fd"},
        {"stdin", "/proc/self/fd/0"},
        {"stdout", "/proc/self/fd/1"},
        {"stderr", "/proc/self/fd/2"},
    };

    for (size_t i = 0; i < sizeof(symlinks) / sizeof(symlinks[0]); i++) {
        char link_path[560];
        snprintf(link_path, sizeof(link_path), "%s/%s", dev_dir, symlinks[i].link_name);
        if (symlink(symlinks[i].target, link_path) < 0 && errno != EEXIST && verbose) {
            log_warn("[Child] symlink %s failed: %s", link_path, strerror(errno));
        }
    }
}

/* Fallback chroot implementation if pivot_root is unavailable */
static int do_chroot(const char *rootfs) {
    if (chroot(rootfs) < 0) {
        log_error("chroot('%s') failed: %s", rootfs, strerror(errno));
        return -1;
    }
    if (chdir("/") < 0) {
        log_error("chdir('/') failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Pivot root to new rootfs */
static int do_pivot_root(const char *new_root) {
    /* Bind mount new_root to itself to ensure it is a mount point */
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) < 0) {
        log_warn("Failed to bind mount rootfs '%s': %s", new_root, strerror(errno));
        return do_chroot(new_root);
    }

    char old_root[512];
    int n = snprintf(old_root, sizeof(old_root), "%s/.old_root", new_root);
    if (n < 0 || (size_t)n >= sizeof(old_root) || ensure_dir_exists(old_root) < 0) {
        log_warn("Failed to create temporary directory '%s'", old_root);
        return do_chroot(new_root);
    }

    /* Perform syscall pivot_root */
    if (syscall(SYS_pivot_root, new_root, old_root) < 0) {
        log_warn("pivot_root failed (%s), trying chroot fallback", strerror(errno));
        rmdir(old_root);
        return do_chroot(new_root);
    }

    if (chdir("/") < 0) {
        log_error("chdir('/') after pivot_root failed: %s", strerror(errno));
        return -1;
    }

    /* Unmount and remove old root */
    if (umount2("/.old_root", MNT_DETACH) < 0) {
        log_warn("Failed to unmount old root: %s", strerror(errno));
    }
    rmdir("/.old_root");
    return 0;
}

static bool has_custom_tmp_bind(const container_opts_t *opts) {
    for (size_t i = 0; i < opts->num_binds; i++) {
        const char *p = opts->binds[i].container_path;
        if (strcmp(p, "/tmp") == 0 || strcmp(p, "tmp") == 0) {
            return true;
        }
    }
    return false;
}

int setup_container_filesystem(const container_opts_t *opts) {
    bool has_rootfs = (opts->rootfs && opts->rootfs[0] != '\0');

    /* Refuse bind mounts without a rootfs so we never create directories on host '/' */
    if (opts->num_binds > 0 && !has_rootfs) {
        log_error("Volume bind mounts (-v) require a custom rootfs (-r) to avoid mutating host '/'");
        return -1;
    }

    /* 1. Make all mounts in child namespace private so mounts don't leak to host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        if (errno == EPERM || errno == EACCES) {
            log_warn("Host kernel restricted unprivileged mount namespace (AppArmor / userns restriction)");
        } else {
            log_warn("Failed to make root mount private: %s", strerror(errno));
        }
    }

    /* 2. Prepare rootfs, /dev, and custom volume binds BEFORE pivot_root */
    if (has_rootfs) {
        setup_dev_nodes(opts->rootfs, opts->verbose);

        /* Mount /tmp inside rootfs first so custom binds under /tmp aren't shadowed */
        if (!has_custom_tmp_bind(opts)) {
            char tmp_dir[512];
            join_rootfs_path(tmp_dir, sizeof(tmp_dir), opts->rootfs, "tmp");
            ensure_dir_exists(tmp_dir);
            if (mount("tmpfs", tmp_dir, "tmpfs", MS_NOSUID | MS_NODEV, "size=64M") < 0 && opts->verbose) {
                log_warn("Mounting %s failed: %s", tmp_dir, strerror(errno));
            }
        }

        for (size_t i = 0; i < opts->num_binds; i++) {
            const volume_bind_t *bind = &opts->binds[i];
            struct stat st_host;
            if (stat(bind->host_path, &st_host) < 0) {
                log_error("Host path '%s' for volume bind does not exist", bind->host_path);
                return -1;
            }

            char target[512];
            join_rootfs_path(target, sizeof(target), opts->rootfs, bind->container_path);
            if (S_ISDIR(st_host.st_mode)) {
                if (ensure_dir_exists(target) < 0) {
                    log_error("Failed to create mount point '%s': %s", target, strerror(errno));
                    return -1;
                }
            } else {
                if (ensure_file_exists(target) < 0) {
                    log_error("Failed to create mount file '%s': %s", target, strerror(errno));
                    return -1;
                }
            }

            if (mount(bind->host_path, target, NULL, MS_BIND | MS_REC, NULL) < 0) {
                log_error("Failed to bind mount '%s' -> '%s': %s",
                          bind->host_path, target, strerror(errno));
                return -1;
            }

            if (bind->read_only) {
                if (mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, NULL) < 0) {
                    log_warn("Failed to remount '%s' read-only: %s", target, strerror(errno));
                }
            }
            if (opts->verbose) {
                log_info("[Child] Mounted Volume: '%s' -> '%s'%s",
                         bind->host_path, bind->container_path,
                         bind->read_only ? " (read-only)" : "");
            }
        }

        if (opts->verbose) {
            log_info("[Child] Switching root filesystem to '%s'", opts->rootfs);
        }
        if (do_pivot_root(opts->rootfs) < 0) {
            return -1;
        }
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

    /* 5. Remount rootfs read-only if requested */
    if (has_rootfs && opts->rootfs_readonly) {
        if (mount(NULL, "/", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
            log_warn("Failed to remount rootfs read-only: %s", strerror(errno));
        } else if (opts->verbose) {
            log_info("[Child] Remounted rootfs read-only");
        }
    }

    /* 6. Change working directory if specified */
    if (opts->cwd && opts->cwd[0] != '\0') {
        if (chdir(opts->cwd) < 0) {
            log_warn("chdir('%s') failed: %s", opts->cwd, strerror(errno));
        }
    }

    return 0;
}
