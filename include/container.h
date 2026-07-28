#ifndef TOY_CONTAINER_H
#define TOY_CONTAINER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>

#define CONTAINER_STACK_SIZE (1024 * 1024) // 1 MB stack for cloned child process
#define DEFAULT_HOSTNAME "toy-container"
#define DEFAULT_CGROUP_NAME "toy-container"

/* Structure representing a Volume Bind Mount (-v /host/path:/container/path) */
typedef struct {
    char *host_path;
    char *container_path;
    bool read_only;
} volume_bind_t;

/* Structure representing an Environment Variable (-e KEY=VALUE) */
typedef struct {
    char *key;
    char *val;
    char *raw;
} env_var_t;

/* Container Configuration Options */
typedef struct {
    char *rootfs;           /* Path to container rootfs (optional) */
    char *hostname;         /* Hostname inside container */
    char *cgroup_name;      /* Name of cgroup v2 directory */
    size_t mem_max_bytes;   /* Memory limit in bytes (0 = no limit) */
    long pids_max;          /* Max processes limit (0 = no limit) */
    char **cmd_argv;        /* Command and arguments to run inside container */
    bool verbose;           /* Enable debug/verbose logs */
    bool isolate_net;       /* Isolate network namespace */

    /* Custom Volume Binds */
    volume_bind_t *binds;
    size_t num_binds;

    /* Environment Variables */
    env_var_t *envs;
    size_t num_envs;
} container_opts_t;

/* Internal Context passed to cloned child process */
typedef struct {
    const container_opts_t *opts;
    int sync_pipe[2];       /* Pipe for parent-child synchronization */
    void *stack;            /* Allocated stack memory for cloned child */
} container_ctx_t;

/* Function Declarations */

/* CLI & Orchestration */
int container_run(const container_opts_t *opts);
void container_opts_init(container_opts_t *opts);
void container_opts_free(container_opts_t *opts);
int container_opts_add_bind(container_opts_t *opts, const char *arg);
int container_opts_add_env(container_opts_t *opts, const char *arg);

/* Namespace Management */
pid_t spawn_container_process(container_ctx_t *ctx);
int setup_id_mappings(pid_t child_pid);

/* Filesystem Setup */
int setup_container_filesystem(const container_opts_t *opts);
int create_minimal_rootfs(const char *target_dir);

/* Cgroups Management */
int setup_cgroups(pid_t child_pid, const container_opts_t *opts, char *out_cgroup_path, size_t path_len);
void cleanup_cgroups(const char *cgroup_path);

/* Utilities */
size_t parse_size_bytes(const char *str);
void log_info(const char *fmt, ...);
void log_success(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif /* TOY_CONTAINER_H */
