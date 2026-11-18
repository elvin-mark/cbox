#include "oci.h"
#include "json.h"
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>

const char *oci_default_state_root(char *buf, size_t buf_len) {
    if (geteuid() == 0) {
        snprintf(buf, buf_len, "/run/cbox");
        return buf;
    }
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') {
        snprintf(buf, buf_len, "%s/cbox", xdg);
        return buf;
    }
    snprintf(buf, buf_len, "/tmp/cbox-state-%u", (unsigned int)getuid());
    return buf;
}

static int mkdir_p(const char *path) {
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

static bool is_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(pid, 0) < 0 && errno == ESRCH) {
        return false;
    }
    /* Check /proc/<pid>/stat to ensure it is not a zombie ('Z') */
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE *f = fopen(stat_path, "r");
    if (!f) {
        return false;
    }
    char buf[512];
    bool alive = true;
    if (fgets(buf, sizeof(buf), f)) {
        char *rparen = strrchr(buf, ')');
        if (rparen && rparen[1] == ' ' && rparen[2] == 'Z') {
            alive = false;
        }
    }
    fclose(f);
    return alive;
}

static int write_state_file(const char *state_dir, const char *id, const char *status,
                            pid_t pid, const char *bundle, const char *cgroup_path) {
    char path[512];
    snprintf(path, sizeof(path), "%s/state.json", state_dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        log_error("Failed to write state file '%s': %s", path, strerror(errno));
        return -1;
    }

    fprintf(f,
            "{\n"
            "  \"ociVersion\": \"%s\",\n"
            "  \"id\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"pid\": %d,\n"
            "  \"bundle\": \"%s\",\n"
            "  \"cgroupPath\": \"%s\"\n"
            "}\n",
            OCI_SPEC_VERSION, id, status, (int)pid, bundle, cgroup_path ? cgroup_path : "");
    fclose(f);
    return 0;
}

static ns_index_t map_oci_ns_type(const char *type) {
    if (!type) return NS_IDX_COUNT;
    if (strcmp(type, "pid") == 0) return NS_IDX_PID;
    if (strcmp(type, "uts") == 0) return NS_IDX_UTS;
    if (strcmp(type, "ipc") == 0) return NS_IDX_IPC;
    if (strcmp(type, "network") == 0) return NS_IDX_NET;
    if (strcmp(type, "mount") == 0) return NS_IDX_MNT;
    if (strcmp(type, "user") == 0) return NS_IDX_USER;
    if (strcmp(type, "cgroup") == 0) return NS_IDX_CGROUP;
    return NS_IDX_COUNT;
}

int oci_load_bundle_config(const char *bundle_dir, const char *container_id,
                           container_opts_t *opts) {
    if (!bundle_dir || !container_id || !opts) {
        return -1;
    }

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", bundle_dir);
    json_value_t *root = json_parse_file(cfg_path);
    if (!root) {
        log_error("Failed to parse OCI config '%s'", cfg_path);
        return -1;
    }

    /* Reset namespaces to only what config.json declares */
    for (size_t i = 0; i < NS_IDX_COUNT; i++) {
        opts->namespaces[i].enabled = false;
        free(opts->namespaces[i].path);
        opts->namespaces[i].path = NULL;
    }
    opts->isolate_net = false;
    opts->clear_env = true;
    opts->quiet = true;

    free(opts->cgroup_name);
    opts->cgroup_name = strdup(container_id);

    /* Hostname */
    const char *hostname = json_as_string(json_obj_get(root, "hostname"));
    if (hostname) {
        free(opts->hostname);
        opts->hostname = strdup(hostname);
    }

    /* Rootfs */
    const json_value_t *root_obj = json_obj_get(root, "root");
    if (root_obj) {
        const char *rpath = json_as_string(json_obj_get(root_obj, "path"));
        if (rpath) {
            if (rpath[0] == '/') {
                opts->rootfs = strdup(rpath);
            } else {
                size_t len = strlen(bundle_dir) + 1 + strlen(rpath) + 1;
                opts->rootfs = malloc(len);
                if (opts->rootfs) {
                    snprintf(opts->rootfs, len, "%s/%s", bundle_dir, rpath);
                }
            }
        }
        opts->rootfs_readonly = json_as_bool(json_obj_get(root_obj, "readonly"), false);
    }

    /* Process */
    const json_value_t *proc = json_obj_get(root, "process");
    if (proc) {
        const json_value_t *args = json_obj_get(proc, "args");
        if (args && args->type == JSON_ARRAY && args->u.array.count > 0) {
            opts->cmd_argv = calloc(args->u.array.count + 1, sizeof(char *));
            if (opts->cmd_argv) {
                for (size_t i = 0; i < args->u.array.count; i++) {
                    const char *a = json_as_string(args->u.array.items[i]);
                    opts->cmd_argv[i] = strdup(a ? a : "");
                }
            }
        }

        const json_value_t *envs = json_obj_get(proc, "env");
        if (envs && envs->type == JSON_ARRAY) {
            for (size_t i = 0; i < envs->u.array.count; i++) {
                const char *e = json_as_string(envs->u.array.items[i]);
                if (e) {
                    container_opts_add_env(opts, e);
                }
            }
        }

        const char *cwd = json_as_string(json_obj_get(proc, "cwd"));
        if (cwd) {
            opts->cwd = strdup(cwd);
        }

        opts->no_new_privs = json_as_bool(json_obj_get(proc, "noNewPrivileges"), false);

        const json_value_t *user = json_obj_get(proc, "user");
        if (user && user->type == JSON_OBJECT) {
            const json_value_t *uid_v = json_obj_get(user, "uid");
            const json_value_t *gid_v = json_obj_get(user, "gid");
            if (uid_v || gid_v) {
                opts->has_uid_gid = true;
                opts->uid = (uid_t)json_as_int64(uid_v, 0);
                opts->gid = (gid_t)json_as_int64(gid_v, 0);
            }
        }
    }

    /* Mounts (handle bind/rbind mounts from OCI spec) */
    const json_value_t *mounts = json_obj_get(root, "mounts");
    if (mounts && mounts->type == JSON_ARRAY) {
        for (size_t i = 0; i < mounts->u.array.count; i++) {
            const json_value_t *m = mounts->u.array.items[i];
            const char *dest = json_as_string(json_obj_get(m, "destination"));
            const char *type = json_as_string(json_obj_get(m, "type"));
            const char *src = json_as_string(json_obj_get(m, "source"));
            const json_value_t *mopts = json_obj_get(m, "options");

            bool is_bind = (type && strcmp(type, "bind") == 0);
            bool is_ro = false;
            if (mopts && mopts->type == JSON_ARRAY) {
                for (size_t j = 0; j < mopts->u.array.count; j++) {
                    const char *opt = json_as_string(mopts->u.array.items[j]);
                    if (!opt) continue;
                    if (strcmp(opt, "bind") == 0 || strcmp(opt, "rbind") == 0) {
                        is_bind = true;
                    } else if (strcmp(opt, "ro") == 0) {
                        is_ro = true;
                    }
                }
            }

            if (is_bind && src && dest) {
                container_opts_add_bind_parts(opts, src, dest, is_ro);
            }
        }
    }

    /* Linux namespaces & resources */
    const json_value_t *linux_obj = json_obj_get(root, "linux");
    if (linux_obj) {
        const json_value_t *nss = json_obj_get(linux_obj, "namespaces");
        if (nss && nss->type == JSON_ARRAY) {
            for (size_t i = 0; i < nss->u.array.count; i++) {
                const json_value_t *ns = nss->u.array.items[i];
                const char *type = json_as_string(json_obj_get(ns, "type"));
                const char *nspath = json_as_string(json_obj_get(ns, "path"));
                ns_index_t idx = map_oci_ns_type(type);
                if (idx < NS_IDX_COUNT) {
                    opts->namespaces[idx].enabled = true;
                    if (nspath && nspath[0] != '\0') {
                        opts->namespaces[idx].path = strdup(nspath);
                    }
                    if (idx == NS_IDX_NET && !nspath) {
                        opts->isolate_net = true;
                    }
                }
            }
        }

        const json_value_t *uid_maps = json_obj_get(linux_obj, "uidMappings");
        const json_value_t *gid_maps = json_obj_get(linux_obj, "gidMappings");
        if (uid_maps && uid_maps->type == JSON_ARRAY && uid_maps->u.array.count > 0 &&
            gid_maps && gid_maps->type == JSON_ARRAY && gid_maps->u.array.count > 0) {
            const json_value_t *um = uid_maps->u.array.items[0];
            const json_value_t *gm = gid_maps->u.array.items[0];
            opts->has_custom_id_map = true;
            opts->uid_map_container = (uint32_t)json_as_int64(json_obj_get(um, "containerID"), 0);
            opts->uid_map_host = (uint32_t)json_as_int64(json_obj_get(um, "hostID"), 0);
            opts->uid_map_size = (uint32_t)json_as_int64(json_obj_get(um, "size"), 1);
            opts->gid_map_container = (uint32_t)json_as_int64(json_obj_get(gm, "containerID"), 0);
            opts->gid_map_host = (uint32_t)json_as_int64(json_obj_get(gm, "hostID"), 0);
            opts->gid_map_size = (uint32_t)json_as_int64(json_obj_get(gm, "size"), 1);
        }

        const json_value_t *res = json_obj_get(linux_obj, "resources");
        if (res) {
            const json_value_t *mem = json_obj_get(res, "memory");
            if (mem) {
                int64_t lim = json_as_int64(json_obj_get(mem, "limit"), 0);
                if (lim > 0) opts->mem_max_bytes = (size_t)lim;
            }
            const json_value_t *pids = json_obj_get(res, "pids");
            if (pids) {
                int64_t lim = json_as_int64(json_obj_get(pids, "limit"), 0);
                if (lim > 0) opts->pids_max = (long)lim;
            }
            const json_value_t *cpu = json_obj_get(res, "cpu");
            if (cpu) {
                int64_t quota = json_as_int64(json_obj_get(cpu, "quota"), 0);
                int64_t period = json_as_int64(json_obj_get(cpu, "period"), 100000);
                if (quota > 0) opts->cpu_quota_us = (long)quota;
                if (period > 0) opts->cpu_period_us = (long)period;
            }
        }
    }

    json_free(root);
    return 0;
}

int oci_cmd_state(const char *state_root, const char *container_id) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/%s/state.json", state_root, container_id);

    json_value_t *st = json_parse_file(state_path);
    if (!st) {
        log_error("Container '%s' does not exist", container_id);
        return 1;
    }

    const char *status = json_as_string(json_obj_get(st, "status"));
    pid_t pid = (pid_t)json_as_int64(json_obj_get(st, "pid"), 0);
    const char *bundle = json_as_string(json_obj_get(st, "bundle"));

    const char *effective_status = status ? status : "stopped";
    if (!is_pid_alive(pid)) {
        effective_status = "stopped";
        pid = 0;
    }

    printf("{\n"
           "  \"ociVersion\": \"%s\",\n"
           "  \"id\": \"%s\",\n"
           "  \"status\": \"%s\",\n"
           "  \"pid\": %d,\n"
           "  \"bundle\": \"%s\"\n"
           "}\n",
           OCI_SPEC_VERSION, container_id, effective_status, (int)pid, bundle ? bundle : "");

    json_free(st);
    return 0;
}

int oci_cmd_create(const char *state_root, const char *container_id,
                   const char *bundle_dir, const char *pid_file) {
    char state_dir[512];
    snprintf(state_dir, sizeof(state_dir), "%s/%s", state_root, container_id);

    struct stat st;
    if (stat(state_dir, &st) == 0) {
        log_error("Container '%s' already exists", container_id);
        return 1;
    }

    if (mkdir_p(state_dir) < 0) {
        log_error("Failed to create state directory '%s': %s", state_dir, strerror(errno));
        return 1;
    }

    char abs_bundle[PATH_MAX];
    const char *bundle_abs = realpath(bundle_dir, abs_bundle) ? abs_bundle : bundle_dir;

    container_opts_t opts;
    container_opts_init(&opts);
    if (oci_load_bundle_config(bundle_abs, container_id, &opts) < 0) {
        container_opts_free(&opts);
        rmdir(state_dir);
        return 1;
    }
    log_set_quiet(true);

    char fifo_path[560];
    snprintf(fifo_path, sizeof(fifo_path), "%s/exec.fifo", state_dir);
    if (mkfifo(fifo_path, 0600) < 0) {
        log_error("Failed to create exec FIFO '%s': %s", fifo_path, strerror(errno));
        container_opts_free(&opts);
        rmdir(state_dir);
        return 1;
    }
    opts.exec_fifo_path = strdup(fifo_path);

    container_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.opts = &opts;
    if (pipe(ctx.sync_pipe) < 0) {
        log_error("pipe() failed: %s", strerror(errno));
        unlink(fifo_path);
        rmdir(state_dir);
        container_opts_free(&opts);
        return 1;
    }

    pid_t child_pid = spawn_container_process(&ctx);
    if (child_pid < 0) {
        close(ctx.sync_pipe[0]);
        close(ctx.sync_pipe[1]);
        unlink(fifo_path);
        rmdir(state_dir);
        container_opts_free(&opts);
        return 1;
    }

    close(ctx.sync_pipe[0]);

    if (setup_id_mappings(child_pid, &opts) < 0) {
        close(ctx.sync_pipe[1]);
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        free(ctx.stack);
        unlink(fifo_path);
        rmdir(state_dir);
        container_opts_free(&opts);
        return 1;
    }

    char cgroup_path[512] = {0};
    setup_cgroups(child_pid, &opts, cgroup_path, sizeof(cgroup_path));

    if (write(ctx.sync_pipe[1], "1", 1) != 1) {
        log_error("Failed to unblock child process");
    }
    close(ctx.sync_pipe[1]);

    if (pid_file && pid_file[0] != '\0') {
        FILE *pf = fopen(pid_file, "w");
        if (pf) {
            fprintf(pf, "%d", (int)child_pid);
            fclose(pf);
        } else {
            log_error("Failed to write pid-file '%s': %s", pid_file, strerror(errno));
        }
    }

    write_state_file(state_dir, container_id, "created", child_pid, bundle_abs, cgroup_path);

    free(ctx.stack);
    container_opts_free(&opts);
    return 0;
}

int oci_cmd_start(const char *state_root, const char *container_id) {
    char state_dir[512];
    char state_path[560];
    snprintf(state_dir, sizeof(state_dir), "%s/%s", state_root, container_id);
    snprintf(state_path, sizeof(state_path), "%s/state.json", state_dir);

    json_value_t *st = json_parse_file(state_path);
    if (!st) {
        log_error("Container '%s' does not exist", container_id);
        return 1;
    }

    const char *status = json_as_string(json_obj_get(st, "status"));
    pid_t pid = (pid_t)json_as_int64(json_obj_get(st, "pid"), 0);
    const char *bundle = json_as_string(json_obj_get(st, "bundle"));
    const char *cgroup_path = json_as_string(json_obj_get(st, "cgroupPath"));

    if (!status || strcmp(status, "created") != 0) {
        log_error("Container '%s' is not in 'created' state", container_id);
        json_free(st);
        return 1;
    }

    char fifo_path[560];
    snprintf(fifo_path, sizeof(fifo_path), "%s/exec.fifo", state_dir);
    int fd = open(fifo_path, O_WRONLY);
    if (fd < 0) {
        log_error("Failed to open exec FIFO '%s': %s", fifo_path, strerror(errno));
        json_free(st);
        return 1;
    }
    if (write(fd, "1", 1) != 1) {
        log_error("Failed to signal container start");
        close(fd);
        json_free(st);
        return 1;
    }
    close(fd);
    unlink(fifo_path);

    write_state_file(state_dir, container_id, "running", pid,
                     bundle ? bundle : "", cgroup_path ? cgroup_path : "");
    json_free(st);
    return 0;
}

int oci_cmd_kill(const char *state_root, const char *container_id, const char *sig_str) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/%s/state.json", state_root, container_id);

    json_value_t *st = json_parse_file(state_path);
    if (!st) {
        log_error("Container '%s' does not exist", container_id);
        return 1;
    }

    pid_t pid = (pid_t)json_as_int64(json_obj_get(st, "pid"), 0);
    json_free(st);

    int sig = sig_str ? parse_signal_name(sig_str) : SIGTERM;
    if (sig < 0) {
        log_error("Invalid signal '%s'", sig_str);
        return 1;
    }

    if (pid <= 0 || !is_pid_alive(pid)) {
        log_error("Container '%s' is not running", container_id);
        return 1;
    }

    if (kill(pid, sig) < 0) {
        log_error("Failed to send signal %d to PID %d: %s", sig, (int)pid, strerror(errno));
        return 1;
    }

    return 0;
}

int oci_cmd_delete(const char *state_root, const char *container_id, bool force) {
    char state_dir[512];
    char state_path[560];
    snprintf(state_dir, sizeof(state_dir), "%s/%s", state_root, container_id);
    snprintf(state_path, sizeof(state_path), "%s/state.json", state_dir);

    json_value_t *st = json_parse_file(state_path);
    if (!st) {
        if (force) {
            return 0;
        }
        log_error("Container '%s' does not exist", container_id);
        return 1;
    }

    pid_t pid = (pid_t)json_as_int64(json_obj_get(st, "pid"), 0);
    const char *cgroup_path = json_as_string(json_obj_get(st, "cgroupPath"));

    if (is_pid_alive(pid)) {
        if (!force) {
            log_error("Container '%s' is still running (use --force to kill)", container_id);
            json_free(st);
            return 1;
        }
        kill(pid, SIGKILL);
        usleep(50000);
    }

    if (cgroup_path && cgroup_path[0] != '\0') {
        cleanup_cgroups(cgroup_path);
    }
    json_free(st);

    char fifo_path[560];
    snprintf(fifo_path, sizeof(fifo_path), "%s/exec.fifo", state_dir);
    unlink(fifo_path);
    unlink(state_path);
    rmdir(state_dir);
    return 0;
}

int oci_cmd_run(const char *state_root, const char *container_id,
                const char *bundle_dir, const char *pid_file) {
    (void)state_root;
    (void)pid_file;

    char abs_bundle[PATH_MAX];
    const char *bundle_abs = realpath(bundle_dir, abs_bundle) ? abs_bundle : bundle_dir;

    container_opts_t opts;
    container_opts_init(&opts);
    if (oci_load_bundle_config(bundle_abs, container_id, &opts) < 0) {
        container_opts_free(&opts);
        return 1;
    }

    int rc = container_run(&opts);
    container_opts_free(&opts);
    return rc >= 0 ? rc : 1;
}

static int join_container_namespaces(pid_t target_pid) {
    static const struct {
        const char *name;
        int clone_type;
    } ns_list[] = {
        {"user", CLONE_NEWUSER},
        {"ipc", CLONE_NEWIPC},
        {"uts", CLONE_NEWUTS},
        {"net", CLONE_NEWNET},
        {"cgroup", CLONE_NEWCGROUP},
        {"pid", CLONE_NEWPID},
        {"mnt", CLONE_NEWNS},
    };

    int fds[7];
    for (size_t i = 0; i < 7; i++) {
        fds[i] = -1;
        char self_path[64], target_path[64];
        snprintf(self_path, sizeof(self_path), "/proc/self/ns/%s", ns_list[i].name);
        snprintf(target_path, sizeof(target_path), "/proc/%d/ns/%s", (int)target_pid, ns_list[i].name);

        struct stat st_self, st_target;
        if (stat(self_path, &st_self) < 0 || stat(target_path, &st_target) < 0) {
            continue;
        }
        /* Skip namespaces already shared with the caller */
        if (st_self.st_dev == st_target.st_dev && st_self.st_ino == st_target.st_ino) {
            continue;
        }

        fds[i] = open(target_path, O_RDONLY);
        if (fds[i] < 0) {
            log_error("Failed to open %s: %s", target_path, strerror(errno));
            for (size_t j = 0; j < i; j++) {
                if (fds[j] >= 0) close(fds[j]);
            }
            return -1;
        }
    }

    for (size_t i = 0; i < 7; i++) {
        if (fds[i] < 0) continue;
        if (setns(fds[i], ns_list[i].clone_type) < 0) {
            log_error("setns(%s) failed: %s", ns_list[i].name, strerror(errno));
            for (size_t j = i; j < 7; j++) {
                if (fds[j] >= 0) close(fds[j]);
            }
            return -1;
        }
        close(fds[i]);
    }

    return 0;
}

int oci_cmd_exec(const char *state_root, const char *container_id,
                 const oci_exec_opts_t *exec_opts) {
    if (!exec_opts) {
        return 1;
    }

    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/%s/state.json", state_root, container_id);

    json_value_t *st = json_parse_file(state_path);
    if (!st) {
        log_error("Container '%s' does not exist", container_id);
        return 1;
    }

    pid_t target_pid = (pid_t)json_as_int64(json_obj_get(st, "pid"), 0);
    const char *cg_str = json_as_string(json_obj_get(st, "cgroupPath"));
    char *cgroup_path = cg_str ? strdup(cg_str) : NULL;
    json_free(st);

    if (!is_pid_alive(target_pid)) {
        log_error("Container '%s' is not running", container_id);
        free(cgroup_path);
        return 1;
    }

    /* Use container_opts_t as a convenient holder for optional process.json overrides */
    container_opts_t proc_opts;
    container_opts_init(&proc_opts);
    bool use_json_args = false;

    if (exec_opts->process_json) {
        json_value_t *proc = json_parse_file(exec_opts->process_json);
        if (!proc) {
            log_error("Failed to parse process JSON '%s'", exec_opts->process_json);
            container_opts_free(&proc_opts);
            free(cgroup_path);
            return 1;
        }

        const json_value_t *args = json_obj_get(proc, "args");
        if (args && args->type == JSON_ARRAY && args->u.array.count > 0) {
            proc_opts.cmd_argv = calloc(args->u.array.count + 1, sizeof(char *));
            if (proc_opts.cmd_argv) {
                for (size_t i = 0; i < args->u.array.count; i++) {
                    const char *a = json_as_string(args->u.array.items[i]);
                    proc_opts.cmd_argv[i] = strdup(a ? a : "");
                }
                use_json_args = true;
            }
        }

        const json_value_t *envs = json_obj_get(proc, "env");
        if (envs && envs->type == JSON_ARRAY) {
            for (size_t i = 0; i < envs->u.array.count; i++) {
                const char *e = json_as_string(envs->u.array.items[i]);
                if (e) container_opts_add_env(&proc_opts, e);
            }
        }

        const char *cwd = json_as_string(json_obj_get(proc, "cwd"));
        if (cwd) proc_opts.cwd = strdup(cwd);
        proc_opts.no_new_privs = json_as_bool(json_obj_get(proc, "noNewPrivileges"), false);

        const json_value_t *user = json_obj_get(proc, "user");
        if (user && user->type == JSON_OBJECT) {
            const json_value_t *uid_v = json_obj_get(user, "uid");
            const json_value_t *gid_v = json_obj_get(user, "gid");
            if (uid_v || gid_v) {
                proc_opts.has_uid_gid = true;
                proc_opts.uid = (uid_t)json_as_int64(uid_v, 0);
                proc_opts.gid = (gid_t)json_as_int64(gid_v, 0);
            }
        }
        json_free(proc);
    }

    char **cmd_argv = use_json_args ? proc_opts.cmd_argv : exec_opts->cmd_argv;
    if (!cmd_argv || !cmd_argv[0]) {
        log_error("No command specified for exec");
        container_opts_free(&proc_opts);
        free(cgroup_path);
        return 1;
    }

    /* Fork a helper process to call setns() so the parent cbox process
     * remains in the host namespaces (avoids mutating caller ns & LSan ptrace hangs). */
    fflush(stdout);
    fflush(stderr);
    pid_t helper = fork();
    if (helper < 0) {
        log_error("fork() failed during exec: %s", strerror(errno));
        container_opts_free(&proc_opts);
        free(cgroup_path);
        return 1;
    }

    if (helper == 0) {
        if (join_container_namespaces(target_pid) < 0) {
            _exit(1);
        }

        pid_t child = fork();
        if (child < 0) {
            log_error("fork() failed during exec: %s", strerror(errno));
            _exit(1);
        }

        if (child == 0) {
            const char *cwd = exec_opts->cwd ? exec_opts->cwd : proc_opts.cwd;
            if (cwd && cwd[0] != '\0') {
                if (chdir(cwd) < 0) {
                    log_warn("[Exec] chdir('%s') failed: %s", cwd, strerror(errno));
                }
            }

            if (exec_opts->no_new_privs || proc_opts.no_new_privs) {
                prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
            }

            if (exec_opts->has_uid_gid) {
                if (setgid(exec_opts->gid) < 0 || setuid(exec_opts->uid) < 0) {
                    log_error("[Exec] setuid/setgid failed: %s", strerror(errno));
                    _exit(1);
                }
            } else if (proc_opts.has_uid_gid) {
                if (setgid(proc_opts.gid) < 0 || setuid(proc_opts.uid) < 0) {
                    log_error("[Exec] setuid/setgid failed: %s", strerror(errno));
                    _exit(1);
                }
            }

            extern char **environ;
            for (size_t i = 0; i < proc_opts.num_envs; i++) {
                setenv(proc_opts.envs[i].key, proc_opts.envs[i].val, 1);
            }
            for (size_t i = 0; i < exec_opts->num_envs; i++) {
                setenv(exec_opts->envs[i].key, exec_opts->envs[i].val, 1);
            }

            execvpe(cmd_argv[0], cmd_argv, environ);
            log_error("[Exec] execvpe('%s') failed: %s", cmd_argv[0], strerror(errno));
            _exit(127);
        }

        /* Attach exec child to container's cgroup if present */
        if (cgroup_path && cgroup_path[0] != '\0') {
            char procs_path[560];
            snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
            int cfd = open(procs_path, O_WRONLY);
            if (cfd >= 0) {
                char pbuf[32];
                int plen = snprintf(pbuf, sizeof(pbuf), "%d\n", (int)child);
                if (write(cfd, pbuf, (size_t)plen) < 0) {
                    /* Best effort */
                }
                close(cfd);
            }
        }

        if (exec_opts->pid_file && exec_opts->pid_file[0] != '\0') {
            FILE *pf = fopen(exec_opts->pid_file, "w");
            if (pf) {
                fprintf(pf, "%d", (int)child);
                fclose(pf);
            }
        }

        if (exec_opts->detach) {
            _exit(0);
        }

        int cstatus = 0;
        pid_t cwp;
        do {
            cwp = waitpid(child, &cstatus, 0);
        } while (cwp < 0 && errno == EINTR);

        if (cwp < 0) {
            _exit(1);
        }
        if (WIFEXITED(cstatus)) {
            _exit(WEXITSTATUS(cstatus));
        }
        if (WIFSIGNALED(cstatus)) {
            _exit(128 + WTERMSIG(cstatus));
        }
        _exit(1);
    }

    container_opts_free(&proc_opts);
    free(cgroup_path);

    int status = 0;
    pid_t wp;
    do {
        wp = waitpid(helper, &status, 0);
    } while (wp < 0 && errno == EINTR);

    if (wp < 0) {
        log_error("waitpid() failed: %s", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
