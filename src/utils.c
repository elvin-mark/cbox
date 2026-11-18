#include "container.h"
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>

#define COLOR_RESET "\033[0m"
#define COLOR_BLUE "\033[34m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED "\033[31m"

static bool g_log_quiet = false;

void log_set_quiet(bool quiet) {
    g_log_quiet = quiet;
}

void log_info(const char *fmt, ...) {
    if (g_log_quiet) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    printf("%s[INFO]%s ", COLOR_BLUE, COLOR_RESET);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void log_success(const char *fmt, ...) {
    if (g_log_quiet) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    printf("%s[SUCCESS]%s ", COLOR_GREEN, COLOR_RESET);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void log_warn(const char *fmt, ...) {
    if (g_log_quiet) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s[WARN]%s ", COLOR_YELLOW, COLOR_RESET);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s[ERROR]%s ", COLOR_RED, COLOR_RESET);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

size_t parse_size_bytes(const char *str) {
    if (!str || *str == '\0') {
        return 0;
    }

    char *endptr = NULL;
    errno = 0;
    double val = strtod(str, &endptr);
    if (errno != 0 || endptr == str || val <= 0) {
        return 0;
    }

    while (*endptr && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    size_t multiplier = 1;
    if (*endptr != '\0') {
        switch (toupper((unsigned char)*endptr)) {
            case 'K':
                multiplier = 1024ULL;
                endptr++;
                break;
            case 'M':
                multiplier = 1024ULL * 1024ULL;
                endptr++;
                break;
            case 'G':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                endptr++;
                break;
            default:
                log_warn("Unknown size unit '%c'", *endptr);
                return 0;
        }
        /* Allow optional trailing 'B' (e.g., "64MB", "512KB") */
        if (*endptr != '\0' && toupper((unsigned char)*endptr) == 'B') {
            endptr++;
        }
        if (*endptr != '\0') {
            log_warn("Invalid trailing characters in size '%s'", str);
            return 0;
        }
    }

    double result = val * (double)multiplier;
    if (result > (double)SIZE_MAX) {
        return 0;
    }
    return (size_t)result;
}

int parse_signal_name(const char *sig_str) {
    if (!sig_str || *sig_str == '\0') {
        return -1;
    }

    char *endptr = NULL;
    errno = 0;
    long num = strtol(sig_str, &endptr, 10);
    if (errno == 0 && *endptr == '\0' && num > 0 && num < 64) {
        return (int)num;
    }

    const char *name = sig_str;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    static const struct {
        const char *name;
        int sig;
    } sig_table[] = {
        {"HUP", SIGHUP},
        {"INT", SIGINT},
        {"QUIT", SIGQUIT},
        {"KILL", SIGKILL},
        {"TERM", SIGTERM},
        {"USR1", SIGUSR1},
        {"USR2", SIGUSR2},
        {"STOP", SIGSTOP},
        {"CONT", SIGCONT},
        {"WINCH", SIGWINCH},
    };

    for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++) {
        if (strcasecmp(name, sig_table[i].name) == 0) {
            return sig_table[i].sig;
        }
    }

    return -1;
}

void container_opts_init(container_opts_t *opts) {
    if (!opts) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->hostname = strdup(DEFAULT_HOSTNAME);
    opts->cgroup_name = strdup(DEFAULT_CGROUP_NAME);
    opts->isolate_net = true;
    opts->cpu_period_us = 100000;

    /* Default namespaces enabled for CLI run mode.
     * Only enable user namespace by default when running unprivileged;
     * as root, mapping only UID 0 breaks CAP_DAC_OVERRIDE on user-owned rootfs. */
    opts->namespaces[NS_IDX_PID].enabled = true;
    opts->namespaces[NS_IDX_UTS].enabled = true;
    opts->namespaces[NS_IDX_IPC].enabled = true;
    opts->namespaces[NS_IDX_NET].enabled = true;
    opts->namespaces[NS_IDX_MNT].enabled = true;
    opts->namespaces[NS_IDX_USER].enabled = (geteuid() != 0);
}

int container_opts_add_bind_parts(container_opts_t *opts, const char *host_path,
                                  const char *container_path, bool read_only) {
    if (!opts || !host_path || !container_path ||
        host_path[0] == '\0' || container_path[0] == '\0') {
        return -1;
    }

    char *h_copy = strdup(host_path);
    char *c_copy = strdup(container_path);
    if (!h_copy || !c_copy) {
        free(h_copy);
        free(c_copy);
        return -1;
    }

    volume_bind_t *new_binds =
        realloc(opts->binds, (opts->num_binds + 1) * sizeof(volume_bind_t));
    if (!new_binds) {
        free(h_copy);
        free(c_copy);
        return -1;
    }

    opts->binds = new_binds;
    opts->binds[opts->num_binds].host_path = h_copy;
    opts->binds[opts->num_binds].container_path = c_copy;
    opts->binds[opts->num_binds].read_only = read_only;
    opts->num_binds++;
    return 0;
}

int container_opts_add_bind(container_opts_t *opts, const char *arg) {
    if (!opts || !arg || arg[0] == '\0') {
        return -1;
    }

    char *dup = strdup(arg);
    if (!dup) {
        return -1;
    }

    char *colon1 = strchr(dup, ':');
    if (!colon1) {
        log_error("Invalid volume bind format '%s'. Expected /host/path:/container/path[:ro]", arg);
        free(dup);
        return -1;
    }

    *colon1 = '\0';
    char *host_p = dup;
    char *target_p = colon1 + 1;
    bool ro = false;

    char *colon2 = strchr(target_p, ':');
    if (colon2) {
        *colon2 = '\0';
        char *opt_str = colon2 + 1;
        if (strcmp(opt_str, "ro") == 0) {
            ro = true;
        } else if (strcmp(opt_str, "rw") != 0) {
            log_error("Unknown volume option '%s' (expected 'ro' or 'rw')", opt_str);
            free(dup);
            return -1;
        }
    }

    if (host_p[0] == '\0' || target_p[0] == '\0') {
        log_error("Empty host or container path in volume bind '%s'", arg);
        free(dup);
        return -1;
    }

    int rc = container_opts_add_bind_parts(opts, host_p, target_p, ro);
    free(dup);
    return rc;
}

int container_opts_add_env(container_opts_t *opts, const char *arg) {
    if (!opts || !arg || arg[0] == '\0') {
        return -1;
    }

    const char *eq = strchr(arg, '=');
    char *key = NULL;
    char *val = NULL;
    char *raw = NULL;

    if (eq) {
        size_t klen = (size_t)(eq - arg);
        if (klen == 0) {
            log_error("Empty environment variable key in '%s'", arg);
            return -1;
        }
        key = strndup(arg, klen);
        val = strdup(eq + 1);
        raw = strdup(arg);
    } else {
        key = strdup(arg);
        const char *env_val = getenv(arg);
        val = strdup(env_val ? env_val : "");
        if (key && val) {
            size_t rlen = strlen(key) + 1 + strlen(val) + 1;
            raw = malloc(rlen);
            if (raw) {
                snprintf(raw, rlen, "%s=%s", key, val);
            }
        }
    }

    if (!key || !val || !raw) {
        free(key);
        free(val);
        free(raw);
        return -1;
    }

    env_var_t *new_envs = realloc(opts->envs, (opts->num_envs + 1) * sizeof(env_var_t));
    if (!new_envs) {
        free(key);
        free(val);
        free(raw);
        return -1;
    }

    opts->envs = new_envs;
    opts->envs[opts->num_envs].key = key;
    opts->envs[opts->num_envs].val = val;
    opts->envs[opts->num_envs].raw = raw;
    opts->num_envs++;
    return 0;
}

void container_opts_free(container_opts_t *opts) {
    if (!opts) {
        return;
    }
    free(opts->rootfs);
    free(opts->hostname);
    free(opts->cwd);
    free(opts->cgroup_name);
    free(opts->exec_fifo_path);
    for (size_t i = 0; i < NS_IDX_COUNT; i++) {
        free(opts->namespaces[i].path);
    }
    if (opts->cmd_argv) {
        for (size_t i = 0; opts->cmd_argv[i] != NULL; i++) {
            free(opts->cmd_argv[i]);
        }
        free(opts->cmd_argv);
    }
    if (opts->binds) {
        for (size_t i = 0; i < opts->num_binds; i++) {
            free(opts->binds[i].host_path);
            free(opts->binds[i].container_path);
        }
        free(opts->binds);
    }
    if (opts->envs) {
        for (size_t i = 0; i < opts->num_envs; i++) {
            free(opts->envs[i].key);
            free(opts->envs[i].val);
            free(opts->envs[i].raw);
        }
        free(opts->envs);
    }
    memset(opts, 0, sizeof(*opts));
}
