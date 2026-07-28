#include "container.h"
#include <sys/stat.h>

static int make_dir(const char *base, const char *sub) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, sub);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return mkdir(path, 0755);
}

int create_minimal_rootfs(const char *target_dir) {
    if (!target_dir || strlen(target_dir) == 0) return -1;

    log_info("Initializing minimal container rootfs at '%s'...", target_dir);

    struct stat st;
    if (stat(target_dir, &st) != 0) {
        if (mkdir(target_dir, 0755) < 0) {
            log_error("Failed to create rootfs directory '%s': %s", target_dir, strerror(errno));
            return -1;
        }
    }

    /* Create standard Linux FHS directories */
    const char *dirs[] = {
        "bin", "sbin", "usr", "usr/bin", "usr/sbin", "lib", "lib64",
        "proc", "sys", "dev", "tmp", "etc", "home", "root"
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (make_dir(target_dir, dirs[i]) < 0) {
            log_warn("Failed to create rootfs subdirectory '%s/%s'", target_dir, dirs[i]);
        }
    }

    log_success("Created directory structure in '%s'", target_dir);
    log_info("Tip: You can extract Alpine Linux or Busybox rootfs into '%s' to run full distro commands!", target_dir);
    return 0;
}
