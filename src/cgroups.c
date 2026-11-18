#include "container.h"

static int write_cgroup_file(const char *cgroup_dir, const char *file, const char *value) {
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s", cgroup_dir, file);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(value);
    ssize_t ret = write(fd, value, len);
    close(fd);
    return (ret == (ssize_t)len) ? 0 : -1;
}

static int enable_controllers(const char *parent_dir) {
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/cgroup.subtree_control", parent_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    const char *controllers = "+memory +pids +cpu";
    ssize_t len = (ssize_t)strlen(controllers);
    ssize_t ret = write(fd, controllers, (size_t)len);
    close(fd);
    return (ret == len) ? 0 : -1;
}

static int try_create_cgroup(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    char procs_file[512];
    int n = snprintf(procs_file, sizeof(procs_file), "%s/cgroup.procs", path);
    if (n < 0 || (size_t)n >= sizeof(procs_file)) {
        return -1;
    }

    if (access(procs_file, W_OK) == 0) {
        return 0;
    }

    rmdir(path);
    return -1;
}

/* Discover a writable cgroup v2 path for current process */
static int find_writable_cgroup_base(char *buf, size_t buf_len, const char *name) {
    /* 1. Try direct system path first (works as root or with delegated root cgroup) */
    int n = snprintf(buf, buf_len, "/sys/fs/cgroup/%s", name);
    if (n > 0 && (size_t)n < buf_len) {
        enable_controllers("/sys/fs/cgroup");
        if (try_create_cgroup(buf) == 0) {
            return 0;
        }
    }

    /* 2. Fallback: Read /proc/self/cgroup to find user delegated cgroup slice */
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) {
        return -1;
    }

    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        /* Cgroups v2 line format: 0::<path> */
        char *colon = strchr(line, ':');
        if (!colon || colon[1] != ':') {
            continue;
        }
        char *rel_path = colon + 2;
        char *nl = strchr(rel_path, '\n');
        if (nl) {
            *nl = '\0';
        }

        char parent_buf[512];
        n = snprintf(parent_buf, sizeof(parent_buf), "/sys/fs/cgroup%s", rel_path);
        if (n > 0 && (size_t)n < sizeof(parent_buf)) {
            enable_controllers(parent_buf);
        }

        n = snprintf(buf, buf_len, "/sys/fs/cgroup%s/%s", rel_path, name);
        if (n > 0 && (size_t)n < buf_len && try_create_cgroup(buf) == 0) {
            found = 0;
        }
        break;
    }

    fclose(f);
    return found;
}

int setup_cgroups(pid_t child_pid, const container_opts_t *opts,
                  char *out_cgroup_path, size_t path_len) {
    if (out_cgroup_path && path_len > 0) {
        out_cgroup_path[0] = '\0';
    }

    if (!opts->mem_max_bytes && opts->pids_max <= 0 && opts->cpu_quota_us <= 0) {
        return 0;
    }

    char cgroup_dir[512];
    const char *name = opts->cgroup_name ? opts->cgroup_name : DEFAULT_CGROUP_NAME;

    if (find_writable_cgroup_base(cgroup_dir, sizeof(cgroup_dir), name) < 0) {
        log_warn("Could not create cgroup v2 directory (insufficient cgroup permissions)");
        return -1;
    }

    if (opts->verbose) {
        log_info("Setting up cgroup v2 at: %s", cgroup_dir);
    }

    /* Set memory limit */
    if (opts->mem_max_bytes > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%zu\n", opts->mem_max_bytes);
        if (write_cgroup_file(cgroup_dir, "memory.max", val) < 0) {
            log_warn("Failed to set memory.max in %s (controller not delegated)", cgroup_dir);
        } else if (opts->verbose) {
            log_info("Cgroup memory limit set to %zu bytes", opts->mem_max_bytes);
        }
    }

    /* Set PIDs limit */
    if (opts->pids_max > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%ld\n", opts->pids_max);
        if (write_cgroup_file(cgroup_dir, "pids.max", val) < 0) {
            log_warn("Failed to set pids.max in %s (controller not delegated)", cgroup_dir);
        } else if (opts->verbose) {
            log_info("Cgroup PIDs limit set to %ld", opts->pids_max);
        }
    }

    /* Set CPU quota (cpu.max format: "<quota> <period>") */
    if (opts->cpu_quota_us > 0) {
        long period = opts->cpu_period_us > 0 ? opts->cpu_period_us : 100000;
        char val[64];
        snprintf(val, sizeof(val), "%ld %ld\n", opts->cpu_quota_us, period);
        if (write_cgroup_file(cgroup_dir, "cpu.max", val) < 0) {
            log_warn("Failed to set cpu.max in %s (controller not delegated)", cgroup_dir);
        } else if (opts->verbose) {
            log_info("Cgroup CPU limit set to %ld/%ld us", opts->cpu_quota_us, period);
        }
    }

    /* Attach child PID to cgroup */
    char pid_str[64];
    snprintf(pid_str, sizeof(pid_str), "%d\n", child_pid);
    if (write_cgroup_file(cgroup_dir, "cgroup.procs", pid_str) < 0) {
        log_warn("Failed to attach PID %d to cgroup.procs", child_pid);
    } else if (opts->verbose) {
        log_info("Attached PID %d to cgroup", child_pid);
    }

    if (out_cgroup_path && path_len > 0) {
        snprintf(out_cgroup_path, path_len, "%s", cgroup_dir);
    }

    return 0;
}

void cleanup_cgroups(const char *cgroup_path) {
    if (!cgroup_path || cgroup_path[0] == '\0') {
        return;
    }
    rmdir(cgroup_path);
}
