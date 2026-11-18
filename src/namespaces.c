#include "container.h"
#include <linux/capability.h>
#include <net/if.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>

static int child_entry(void *arg);
static volatile sig_atomic_t g_workload_pid = -1;

static void init_forward_signal(int sig) {
    if (g_workload_pid > 0) {
        kill((pid_t)g_workload_pid, sig);
    }
}

int setup_loopback_interface(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "lo");

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        close(sock);
        return -1;
    }

    ifr.ifr_flags |= (short)(IFF_UP | IFF_RUNNING);
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

static void drop_dangerous_capabilities(bool verbose) {
    static const int drop_list[] = {
        CAP_SYS_BOOT,
        CAP_SYS_MODULE,
        CAP_SYS_RAWIO,
        CAP_SYS_TIME,
        CAP_SYS_PTRACE,
        CAP_SYS_PACCT,
        CAP_MKNOD,
    };

    for (size_t i = 0; i < sizeof(drop_list) / sizeof(drop_list[0]); i++) {
        if (prctl(PR_CAPBSET_DROP, drop_list[i], 0, 0, 0) < 0 && verbose) {
            log_warn("[Child] Failed to drop capability %d: %s", drop_list[i], strerror(errno));
        }
    }
}

static int join_existing_namespaces(const container_opts_t *opts) {
    static const struct {
        ns_index_t idx;
        int clone_type;
        const char *name;
    } ns_table[] = {
        {NS_IDX_USER, CLONE_NEWUSER, "user"},
        {NS_IDX_MNT, CLONE_NEWNS, "mnt"},
        {NS_IDX_PID, CLONE_NEWPID, "pid"},
        {NS_IDX_UTS, CLONE_NEWUTS, "uts"},
        {NS_IDX_IPC, CLONE_NEWIPC, "ipc"},
        {NS_IDX_NET, CLONE_NEWNET, "net"},
        {NS_IDX_CGROUP, CLONE_NEWCGROUP, "cgroup"},
    };

    for (size_t i = 0; i < sizeof(ns_table) / sizeof(ns_table[0]); i++) {
        const ns_spec_t *spec = &opts->namespaces[ns_table[i].idx];
        if (!spec->enabled || !spec->path) {
            continue;
        }
        int fd = open(spec->path, O_RDONLY);
        if (fd < 0) {
            log_error("[Child] Failed to open %s namespace '%s': %s",
                      ns_table[i].name, spec->path, strerror(errno));
            return -1;
        }
        if (setns(fd, ns_table[i].clone_type) < 0) {
            log_error("[Child] setns(%s, '%s') failed: %s",
                      ns_table[i].name, spec->path, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
        if (opts->verbose) {
            log_info("[Child] Joined %s namespace: %s", ns_table[i].name, spec->path);
        }
    }

    return 0;
}

/* Set up User Namespace UID and GID mapping (Parent side) */
int setup_id_mappings(pid_t child_pid, const container_opts_t *opts) {
    if (!opts->namespaces[NS_IDX_USER].enabled || opts->namespaces[NS_IDX_USER].path != NULL) {
        return 0;
    }

    uint32_t uid_c = opts->has_custom_id_map ? opts->uid_map_container : 0;
    uint32_t uid_h = opts->has_custom_id_map ? opts->uid_map_host : (uint32_t)getuid();
    uint32_t uid_sz = opts->has_custom_id_map ? opts->uid_map_size : 1;

    uint32_t gid_c = opts->has_custom_id_map ? opts->gid_map_container : 0;
    uint32_t gid_h = opts->has_custom_id_map ? opts->gid_map_host : (uint32_t)getgid();
    uint32_t gid_sz = opts->has_custom_id_map ? opts->gid_map_size : 1;

    char path[256];
    char map_buf[64];
    int fd, len;

    /* Write UID map */
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    len = snprintf(map_buf, sizeof(map_buf), "%u %u %u\n", uid_c, uid_h, uid_sz);
    if (write(fd, map_buf, (size_t)len) != len) {
        log_error("Failed to write to %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    /* Disable setgroups (required for unprivileged GID mapping) */
    snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "deny\n", 5) != 5) {
            log_warn("Failed to write deny to %s: %s", path, strerror(errno));
        }
        close(fd);
    }

    /* Write GID map */
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    len = snprintf(map_buf, sizeof(map_buf), "%u %u %u\n", gid_c, gid_h, gid_sz);
    if (write(fd, map_buf, (size_t)len) != len) {
        log_error("Failed to write to %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

pid_t spawn_container_process(container_ctx_t *ctx) {
    if (!ctx || !ctx->opts) {
        return -1;
    }

    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (!stack) {
        log_error("Failed to allocate stack memory for container process");
        return -1;
    }
    ctx->stack = stack;

    /* Align stack pointer to 16-byte boundary (stack grows downwards) */
    char *stack_top = stack + CONTAINER_STACK_SIZE;
    stack_top = (char *)((uintptr_t)stack_top & ~(uintptr_t)0xF);

    int flags = SIGCHLD;
    const container_opts_t *opts = ctx->opts;

    if (opts->namespaces[NS_IDX_PID].enabled && !opts->namespaces[NS_IDX_PID].path) {
        flags |= CLONE_NEWPID;
    }
    if (opts->namespaces[NS_IDX_UTS].enabled && !opts->namespaces[NS_IDX_UTS].path) {
        flags |= CLONE_NEWUTS;
    }
    if (opts->namespaces[NS_IDX_MNT].enabled && !opts->namespaces[NS_IDX_MNT].path) {
        flags |= CLONE_NEWNS;
    }
    if (opts->namespaces[NS_IDX_IPC].enabled && !opts->namespaces[NS_IDX_IPC].path) {
        flags |= CLONE_NEWIPC;
    }
    if (opts->namespaces[NS_IDX_USER].enabled && !opts->namespaces[NS_IDX_USER].path) {
        flags |= CLONE_NEWUSER;
    }
    if (opts->namespaces[NS_IDX_NET].enabled && !opts->namespaces[NS_IDX_NET].path &&
        opts->isolate_net) {
        flags |= CLONE_NEWNET;
    }
    if (opts->namespaces[NS_IDX_CGROUP].enabled && !opts->namespaces[NS_IDX_CGROUP].path) {
        flags |= CLONE_NEWCGROUP;
    }

    /* Flush stdio buffers before cloning so buffered output is not duplicated */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = clone(child_entry, stack_top, flags, ctx);
    if (pid == -1) {
        log_error("clone() failed: %s", strerror(errno));
        free(stack);
        ctx->stack = NULL;
        return -1;
    }

    return pid;
}

static int run_pid1_init_reaper(const container_opts_t *opts, char **environ_ptr) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = init_forward_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    pid_t workload = fork();
    if (workload < 0) {
        log_error("[Init] fork() failed: %s", strerror(errno));
        return 1;
    }

    if (workload == 0) {
        execvpe(opts->cmd_argv[0], opts->cmd_argv, environ_ptr);
        log_error("[Child] execvpe('%s') failed: %s", opts->cmd_argv[0], strerror(errno));
        _exit(127);
    }

    g_workload_pid = workload;
    int exit_code = 0;

    for (;;) {
        int status = 0;
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (reaped == workload) {
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            }
            break;
        }
    }

    return exit_code;
}

static int child_entry(void *arg) {
    container_ctx_t *ctx = (container_ctx_t *)arg;
    const container_opts_t *opts = ctx->opts;

    /* Close write end of synchronization pipe in child */
    close(ctx->sync_pipe[1]);

    /* Wait for parent to write UID/GID map and configure cgroups */
    char sync_byte;
    if (read(ctx->sync_pipe[0], &sync_byte, 1) != 1) {
        log_error("[Child] Synchronization failed, parent exited or closed pipe");
        _exit(1);
    }
    close(ctx->sync_pipe[0]);

    /* Join any pre-existing namespaces (e.g. Kubernetes pause pod net/ipc ns) */
    if (join_existing_namespaces(opts) < 0) {
        _exit(1);
    }

    if (opts->verbose) {
        log_info("[Child] Inside container namespaces (PID: %d, UID: %d, GID: %d)",
                 getpid(), getuid(), getgid());
    }

    /* Set UTS Hostname if we have our own UTS namespace */
    if (opts->namespaces[NS_IDX_UTS].enabled && !opts->namespaces[NS_IDX_UTS].path &&
        opts->hostname && opts->hostname[0] != '\0') {
        if (sethostname(opts->hostname, strlen(opts->hostname)) < 0) {
            log_warn("[Child] Failed to set hostname to '%s': %s", opts->hostname, strerror(errno));
        } else if (opts->verbose) {
            log_info("[Child] Set hostname to '%s'", opts->hostname);
        }
    }

    /* Bring up loopback interface in isolated netns */
    if (opts->namespaces[NS_IDX_NET].enabled && !opts->namespaces[NS_IDX_NET].path &&
        opts->isolate_net) {
        if (setup_loopback_interface() < 0 && opts->verbose) {
            log_warn("[Child] Failed to bring up loopback 'lo': %s", strerror(errno));
        }
    }

    /* Set up Container Filesystem (mount points, /dev, /proc, /sys, rootfs) */
    if (setup_container_filesystem(opts) < 0) {
        log_error("[Child] Failed to set up container filesystem");
        _exit(1);
    }

    /* OCI create/start handshake: block on exec.fifo until 'cbox start' */
    if (opts->exec_fifo_path && opts->exec_fifo_path[0] != '\0') {
        int fifo_fd = open(opts->exec_fifo_path, O_RDONLY);
        if (fifo_fd < 0) {
            log_error("[Child] Failed to open exec FIFO '%s': %s",
                      opts->exec_fifo_path, strerror(errno));
            _exit(1);
        }
        char start_byte;
        ssize_t n = read(fifo_fd, &start_byte, 1);
        close(fifo_fd);
        if (n != 1) {
            _exit(1);
        }
    }

    /* Drop dangerous capabilities & lock down privileges if requested */
    if (opts->drop_caps) {
        drop_dangerous_capabilities(opts->verbose);
    }
    if (opts->no_new_privs) {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 && opts->verbose) {
            log_warn("[Child] PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
        }
    }

    /* Drop to requested UID/GID if specified */
    if (opts->has_uid_gid) {
        if (setgid(opts->gid) < 0 || setuid(opts->uid) < 0) {
            log_error("[Child] Failed to setuid/setgid (%u:%u): %s",
                      opts->uid, opts->gid, strerror(errno));
            _exit(1);
        }
    }

    /* Set Environment Variables (-e KEY=VALUE or OCI process.env) */
    if (opts->clear_env) {
        clearenv();
    }
    extern char **environ;
    for (size_t i = 0; i < opts->num_envs; i++) {
        setenv(opts->envs[i].key, opts->envs[i].val, 1);
        if (opts->verbose) {
            log_info("[Child] Env: %s=%s", opts->envs[i].key, opts->envs[i].val);
        }
    }

    if (!opts->cmd_argv || !opts->cmd_argv[0]) {
        log_error("[Child] No command provided to execute inside container");
        _exit(1);
    }

    if (opts->verbose) {
        log_info("[Child] Executing: %s", opts->cmd_argv[0]);
    }

    if (opts->init_reaper) {
        _exit(run_pid1_init_reaper(opts, environ));
    }

    execvpe(opts->cmd_argv[0], opts->cmd_argv, environ);

    log_error("[Child] execvpe('%s') failed: %s", opts->cmd_argv[0], strerror(errno));
    _exit(127);
}
