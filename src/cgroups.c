#include "container.h"

static int write_cgroup_file(const char *cgroup_dir, const char *file, const char *value) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cgroup_dir, file);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t len = strlen(value);
    ssize_t ret = write(fd, value, len);
    close(fd);
    return (ret == len) ? 0 : -1;
}

static int enable_controllers(const char *parent_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", parent_dir);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, "+memory +pids", 13);
    close(fd);
    return 0;
}

/* Discover a writable cgroup v2 path for current process */
static int find_writable_cgroup_base(char *buf, size_t buf_len, const char *name) {
    /* Try direct system path first */
    snprintf(buf, buf_len, "/sys/fs/cgroup/%s", name);
    if (mkdir(buf, 0755) == 0 || errno == EEXIST) {
        /* Test writability by checking cgroup.procs */
        char test_file[512];
        snprintf(test_file, sizeof(test_file), "%s/cgroup.procs", buf);
        if (access(test_file, W_OK) == 0) {
            return 0;
        }
        rmdir(buf);
    }

    /* Fallback: Read /proc/self/cgroup to find user delegated cgroup slice */
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            /* Cgroups v2 line format: 0::<path> */
            char *colon = strchr(line, ':');
            if (colon && colon[1] == ':') {
                char *rel_path = colon + 2;
                // strip trailing newline
                char *nl = strchr(rel_path, '\n');
                if (nl) *nl = '\0';

                enable_controllers("/sys/fs/cgroup");
                if (mkdir(buf, 0755) == 0 || errno == EEXIST) {
                    return 0;
                }
                char parent_buf[512];
                snprintf(parent_buf, sizeof(parent_buf), "/sys/fs/cgroup%s", rel_path);
                enable_controllers(parent_buf);
                snprintf(buf, buf_len, "/sys/fs/cgroup%s/%s", rel_path, name);
                fclose(f);
                if (mkdir(buf, 0755) == 0 || errno == EEXIST) {
                    return 0;
                }
                return -1;
            }
        }
        fclose(f);
    }

    return -1;
}

int setup_cgroups(pid_t child_pid, const container_opts_t *opts, char *out_cgroup_path, size_t path_len) {
    if (!opts->mem_max_bytes && opts->pids_max <= 0) {
        /* No cgroup limits configured */
        if (out_cgroup_path) out_cgroup_path[0] = '\0';
        return 0;
    }

    char cgroup_dir[512];
    const char *name = opts->cgroup_name ? opts->cgroup_name : DEFAULT_CGROUP_NAME;

    if (find_writable_cgroup_base(cgroup_dir, sizeof(cgroup_dir), name) < 0) {
        log_warn("Could not create cgroup v2 directory (insufficient cgroup permissions)");
        if (out_cgroup_path) out_cgroup_path[0] = '\0';
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
            log_warn("Failed to set memory.max to %s in %s", val, cgroup_dir);
        } else if (opts->verbose) {
            log_info("Cgroup memory limit set to %zu bytes", opts->mem_max_bytes);
        }
    }

    /* Set PIDs limit */
    if (opts->pids_max > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%ld\n", opts->pids_max);
        if (write_cgroup_file(cgroup_dir, "pids.max", val) < 0) {
            log_warn("Failed to set pids.max to %s in %s", val, cgroup_dir);
        } else if (opts->verbose) {
            log_info("Cgroup PIDs limit set to %ld", opts->pids_max);
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
        strncpy(out_cgroup_path, cgroup_dir, path_len - 1);
        out_cgroup_path[path_len - 1] = '\0';
    }

    return 0;
}

void cleanup_cgroups(const char *cgroup_path) {
    if (!cgroup_path || strlen(cgroup_path) == 0) return;
    /* Clean up empty cgroup directory */
    rmdir(cgroup_path);
}
