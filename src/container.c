#include "container.h"
#include "oci.h"
#include <signal.h>

static volatile sig_atomic_t g_child_pid = -1;

static void forward_signal_handler(int sig) {
    if (g_child_pid > 0) {
        kill((pid_t)g_child_pid, sig);
    }
}

int container_run(const container_opts_t *opts) {
    if (!opts || !opts->cmd_argv || !opts->cmd_argv[0]) {
        log_error("No command specified to run inside container");
        return -1;
    }

    log_set_quiet(opts->quiet);

    log_info("Starting cbox container...");
    if (opts->verbose) {
        log_info("  Hostname : %s", opts->hostname ? opts->hostname : DEFAULT_HOSTNAME);
        if (opts->rootfs) {
            log_info("  RootFS   : %s%s", opts->rootfs, opts->rootfs_readonly ? " (ro)" : "");
        }
        if (opts->mem_max_bytes) {
            log_info("  Mem Max  : %zu bytes", opts->mem_max_bytes);
        }
        if (opts->pids_max) {
            log_info("  PIDs Max : %ld", opts->pids_max);
        }
        if (opts->cpu_quota_us > 0) {
            log_info("  CPU Max  : %ld/%ld us", opts->cpu_quota_us, opts->cpu_period_us);
        }
    }

    container_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
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
    if (setup_id_mappings(child_pid, opts) < 0) {
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

    /* Register running container in state root so `cbox list` and `cbox exec` can see it */
    bool registered = false;
    if (opts->state_root && opts->container_id) {
        if (oci_register_running_state(opts->state_root, opts->container_id, child_pid,
                                       opts->bundle_path, cgroup_path, opts->pid_file) == 0) {
            registered = true;
        }
    }

    /* Forward SIGINT, SIGTERM, SIGHUP from parent to container process */
    g_child_pid = child_pid;
    struct sigaction sa, old_int, old_term, old_hup;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
    sigaction(SIGHUP, &sa, &old_hup);

    /* Wait for container execution to finish */
    int status = 0;
    pid_t wp;
    do {
        wp = waitpid(child_pid, &status, 0);
    } while (wp < 0 && errno == EINTR);

    /* Restore signal handlers */
    g_child_pid = -1;
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP, &old_hup, NULL);

    /* Clean up state registration, cgroup & stack memory on all paths */
    if (registered) {
        oci_unregister_state(opts->state_root, opts->container_id);
    }
    cleanup_cgroups(cgroup_path);
    free(ctx.stack);

    if (wp < 0) {
        log_error("waitpid() failed: %s", strerror(errno));
        return -1;
    }

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
