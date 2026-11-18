#ifndef TOY_OCI_H
#define TOY_OCI_H

#include "container.h"

#define OCI_SPEC_VERSION "1.0.2"

typedef struct {
    const char *process_json;
    const char *pid_file;
    const char *cwd;
    bool detach;
    bool no_new_privs;
    bool has_uid_gid;
    uid_t uid;
    gid_t gid;
    env_var_t *envs;
    size_t num_envs;
    char **cmd_argv;
} oci_exec_opts_t;

const char *oci_default_state_root(char *buf, size_t buf_len);

int oci_load_bundle_config(const char *bundle_dir, const char *container_id,
                           container_opts_t *opts);

int oci_cmd_state(const char *state_root, const char *container_id);
int oci_cmd_create(const char *state_root, const char *container_id,
                   const char *bundle_dir, const char *pid_file);
int oci_cmd_start(const char *state_root, const char *container_id);
int oci_cmd_kill(const char *state_root, const char *container_id, const char *sig_str);
int oci_cmd_delete(const char *state_root, const char *container_id, bool force);
int oci_cmd_run(const char *state_root, const char *container_id,
                const char *bundle_dir, const char *pid_file);
int oci_cmd_exec(const char *state_root, const char *container_id,
                 const oci_exec_opts_t *exec_opts);

#endif /* TOY_OCI_H */
