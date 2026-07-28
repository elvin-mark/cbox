#include "container.h"

int container_run(const container_opts_t *opts) {
    if (!opts || !opts->cmd_argv || !opts->cmd_argv[0]) {
        log_error("No command specified to run inside container");
        return -1;
    }

    log_info("Starting toy container...");
    if (opts->verbose) {
        log_info("  Hostname : %s", opts->hostname ? opts->hostname : DEFAULT_HOSTNAME);
        if (opts->rootfs) {
            log_info("  RootFS   : %s", opts->rootfs);
        }
        if (opts->mem_max_bytes) {
            log_info("  Mem Max  : %zu bytes", opts->mem_max_bytes);
        }
        if (opts->pids_max) {
            log_info("  PIDs Max : %ld", opts->pids_max);
        }
    }

    container_ctx_t ctx;
    ctx.opts = opts;

    if (pipe(ctx.sync_pipe) < 0) {
        log_error("Failed to create synchronization pipe: %s", strerror(errno));
        return -1;
    }

    /* Spawn cloned child process in new namespaces */
    pid_t child_pid = spawn_container_process(&ctx);
    if (child_pid < 0) {
        close(ctx.sync_pipe[0]);
        close(ctx.sync_pipe[1]);
        return -1;
    }

    log_info("Container process spawned with Host PID: %d", child_pid);

    /* Close read end of sync pipe in parent */
    close(ctx.sync_pipe[0]);

    /* Configure User Namespace UID/GID mappings */
    if (setup_id_mappings(child_pid) < 0) {
        log_error("Parent failed to setup UID/GID mappings");
        close(ctx.sync_pipe[1]);
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        free(ctx.stack);
        return -1;
    }

    /* Configure Cgroups v2 resource limits */
    char cgroup_path[512] = {0};
    setup_cgroups(child_pid, opts, cgroup_path, sizeof(cgroup_path));

    /* Unblock child process */
    if (write(ctx.sync_pipe[1], "1", 1) != 1) {
        log_error("Failed to send sync byte to child process");
    }
    close(ctx.sync_pipe[1]);

    /* Wait for container execution to finish */
    int status = 0;
    if (waitpid(child_pid, &status, 0) < 0) {
        log_error("waitpid() failed: %s", strerror(errno));
        cleanup_cgroups(cgroup_path);
        return -1;
    }

    /* Clean up cgroup & stack memory */
    cleanup_cgroups(cgroup_path);
    free(ctx.stack);

    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            log_success("Container process exited normally (code 0)");
        } else {
            log_warn("Container process exited with status code %d", exit_code);
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        log_warn("Container process terminated by signal %d (%s)", sig, strsignal(sig));
        exit_code = 128 + sig;
    }

    return exit_code;
}
