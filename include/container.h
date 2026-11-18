#ifndef CBOX_H
#define CBOX_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONTAINER_STACK_SIZE (1024 * 1024) /* 1 MB stack for cloned child */
#define DEFAULT_HOSTNAME "cbox"
#define DEFAULT_CGROUP_NAME "cbox"

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define PRINTF_FMT(fmt_idx, arg_idx)
#endif

/* Structure representing a Volume Bind Mount (-v /host/path:/container/path[:ro]) */
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

/* Namespace configuration (supports creating new or joining existing via path) */
typedef enum {
    NS_IDX_PID = 0,
    NS_IDX_UTS,
    NS_IDX_IPC,
    NS_IDX_NET,
    NS_IDX_MNT,
    NS_IDX_USER,
    NS_IDX_CGROUP,
    NS_IDX_COUNT
} ns_index_t;

typedef struct {
    bool enabled; /* Create new namespace if path == NULL */
    char *path;   /* Join existing namespace via setns() if non-NULL */
} ns_spec_t;

/* Container Configuration Options */
typedef struct {
    char *rootfs;         /* Path to container rootfs (optional) */
    bool rootfs_readonly; /* Remount rootfs read-only after setup */
    char *hostname;       /* Hostname inside container */
    char *cwd;            /* Working directory inside container */
    char *cgroup_name;    /* Name of cgroup v2 directory */
    size_t mem_max_bytes; /* Memory limit in bytes (0 = no limit) */
    long pids_max;        /* Max processes limit (0 = no limit) */
    long cpu_quota_us;    /* CPU quota in microseconds per period (0 = no limit) */
    long cpu_period_us;   /* CPU period in microseconds (default 100000) */
    char **cmd_argv;      /* Command and arguments to run inside container */
    bool verbose;         /* Enable debug/verbose logs */
    bool quiet;           /* Suppress informational logs (useful for OCI JSON output) */
    bool isolate_net;     /* Isolate network namespace (legacy CLI toggle) */
    bool init_reaper;     /* Run PID 1 init reaper inside container */
    bool no_new_privs;    /* Set PR_SET_NO_NEW_PRIVS */
    bool drop_caps;       /* Drop dangerous Linux capabilities */
    bool has_uid_gid;     /* Explicitly set UID/GID before exec */
    uid_t uid;
    gid_t gid;

    /* Namespace specs (populated by CLI defaults or OCI config.json) */
    ns_spec_t namespaces[NS_IDX_COUNT];

    /* Optional UserNS mappings (defaults to 0 -> current uid/gid if empty) */
    uint32_t uid_map_container;
    uint32_t uid_map_host;
    uint32_t uid_map_size;
    uint32_t gid_map_container;
    uint32_t gid_map_host;
    uint32_t gid_map_size;
    bool has_custom_id_map;

    /* Custom Volume Binds */
    volume_bind_t *binds;
    size_t num_binds;

    /* Environment Variables */
    env_var_t *envs;
    size_t num_envs;
    bool clear_env; /* Clear host environment before applying envs (OCI mode) */

    /* OCI lifecycle synchronization: if non-NULL, child waits on this FIFO before exec */
    char *exec_fifo_path;
} container_opts_t;

/* Internal Context passed to cloned child process */
typedef struct {
    const container_opts_t *opts;
    int sync_pipe[2]; /* Pipe for parent-child UID/cgroup synchronization */
    void *stack;      /* Allocated stack memory for cloned child */
} container_ctx_t;

/* Function Declarations */

/* CLI & Orchestration */
int container_run(const container_opts_t *opts);
void container_opts_init(container_opts_t *opts);
void container_opts_free(container_opts_t *opts);
int container_opts_add_bind(container_opts_t *opts, const char *arg);
int container_opts_add_bind_parts(container_opts_t *opts, const char *host_path,
                                  const char *container_path, bool read_only);
int container_opts_add_env(container_opts_t *opts, const char *arg);

/* Namespace & Process Management */
pid_t spawn_container_process(container_ctx_t *ctx);
int setup_id_mappings(pid_t child_pid, const container_opts_t *opts);
int setup_loopback_interface(void);

/* Filesystem Setup */
int setup_container_filesystem(const container_opts_t *opts);
int create_minimal_rootfs(const char *target_dir);

/* Cgroups Management */
int setup_cgroups(pid_t child_pid, const container_opts_t *opts,
                  char *out_cgroup_path, size_t path_len);
void cleanup_cgroups(const char *cgroup_path);

/* Utilities */
size_t parse_size_bytes(const char *str);
int parse_signal_name(const char *sig_str);
void log_set_quiet(bool quiet);
void log_info(const char *fmt, ...) PRINTF_FMT(1, 2);
void log_success(const char *fmt, ...) PRINTF_FMT(1, 2);
void log_warn(const char *fmt, ...) PRINTF_FMT(1, 2);
void log_error(const char *fmt, ...) PRINTF_FMT(1, 2);

#endif /* CBOX_H */
