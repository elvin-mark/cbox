#include "container.h"

/* Forward declaration of child process entry function */
static int child_entry(void *arg);

/* Set up User Namespace UID and GID mapping (Parent side) */
int setup_id_mappings(pid_t child_pid) {
    uid_t uid = getuid();
    gid_t gid = getgid();

    char path[256];
    char map_buf[64];
    int fd, len;

    /* Write UID map: Map container UID 0 to host UID */
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("Failed to open %path: %s", path, strerror(errno));
        return -1;
    }
    len = snprintf(map_buf, sizeof(map_buf), "0 %d 1\n", uid);
    if (write(fd, map_buf, len) != len) {
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

    /* Write GID map: Map container GID 0 to host GID */
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    len = snprintf(map_buf, sizeof(map_buf), "0 %d 1\n", gid);
    if (write(fd, map_buf, len) != len) {
        log_error("Failed to write to %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

pid_t spawn_container_process(container_ctx_t *ctx) {
    if (!ctx || !ctx->opts) return -1;

    /* Allocate stack for cloned child process */
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (!stack) {
        log_error("Failed to allocate stack memory for container process");
        return -1;
    }
    ctx->stack = stack;

    /* Align stack pointer to 16-byte boundary (stack grows downwards) */
    char *stack_top = stack + CONTAINER_STACK_SIZE;
    stack_top = (char *)((uintptr_t)stack_top & ~0xF);

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUSER | SIGCHLD;
    if (ctx->opts->isolate_net) {
        flags |= CLONE_NEWNET;
    }

    pid_t pid = clone(child_entry, stack_top, flags, ctx);
    if (pid == -1) {
        log_error("clone() failed: %s", strerror(errno));
        free(stack);
        return -1;
    }

    /* Note: stack memory remains allocated until parent reaps child or exits */
    return pid;
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

    if (opts->verbose) {
        log_info("[Child] Inside container namespaces (PID: %d, UID: %d, GID: %d)",
                 getpid(), getuid(), getgid());
    }

    /* Set UTS Hostname */
    if (opts->hostname && strlen(opts->hostname) > 0) {
        if (sethostname(opts->hostname, strlen(opts->hostname)) < 0) {
            log_warn("[Child] Failed to set hostname to '%s': %s", opts->hostname, strerror(errno));
        } else if (opts->verbose) {
            log_info("[Child] Set hostname to '%s'", opts->hostname);
        }
    }

    /* Set up Container Filesystem (mount points, proc, dev, rootfs) */
    setup_container_filesystem(opts);

    /* Set Environment Variables (-e KEY=VALUE) */
    extern char **environ;
    for (size_t i = 0; i < opts->num_envs; i++) {
        setenv(opts->envs[i].key, opts->envs[i].val, 1);
        if (opts->verbose) {
            log_info("[Child] Env: %s=%s", opts->envs[i].key, opts->envs[i].val);
        }
    }

    /* Execute the requested command */
    if (!opts->cmd_argv || !opts->cmd_argv[0]) {
        log_error("[Child] No command provided to execute inside container");
        _exit(1);
    }

    if (opts->verbose) {
        log_info("[Child] Executing: %s", opts->cmd_argv[0]);
    }

    execvpe(opts->cmd_argv[0], opts->cmd_argv, environ);

    /* If execvpe returns, an error occurred */
    log_error("[Child] execvpe('%s') failed: %s", opts->cmd_argv[0], strerror(errno));
    _exit(127);
}
