#include "container.h"
#include <stdarg.h>
#include <ctype.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RED     "\033[31m"

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("%s[INFO]%s ", COLOR_BLUE, COLOR_RESET);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void log_success(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("%s[SUCCESS]%s ", COLOR_GREEN, COLOR_RESET);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void log_warn(const char *fmt, ...) {
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
    if (errno != 0 || val <= 0) {
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
                break;
            case 'M':
                multiplier = 1024ULL * 1024ULL;
                break;
            case 'G':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                break;
            default:
                log_warn("Unknown size unit '%c', defaulting to bytes", *endptr);
                break;
        }
    }

    return (size_t)(val * multiplier);
}

void container_opts_init(container_opts_t *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->hostname = strdup(DEFAULT_HOSTNAME);
    opts->cgroup_name = strdup(DEFAULT_CGROUP_NAME);
    opts->isolate_net = true;
}

int container_opts_add_bind(container_opts_t *opts, const char *arg) {
    if (!opts || !arg || strlen(arg) == 0) return -1;

    char *dup = strdup(arg);
    if (!dup) return -1;

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
        }
    }

    volume_bind_t *new_binds = realloc(opts->binds, (opts->num_binds + 1) * sizeof(volume_bind_t));
    if (!new_binds) {
        free(dup);
        return -1;
    }

    opts->binds = new_binds;
    opts->binds[opts->num_binds].host_path = strdup(host_p);
    opts->binds[opts->num_binds].container_path = strdup(target_p);
    opts->binds[opts->num_binds].read_only = ro;
    opts->num_binds++;

    free(dup);
    return 0;
}

int container_opts_add_env(container_opts_t *opts, const char *arg) {
    if (!opts || !arg || strlen(arg) == 0) return -1;

    char *eq = strchr(arg, '=');
    char *key = NULL;
    char *val = NULL;
    char *raw = NULL;

    if (eq) {
        size_t klen = eq - arg;
        key = strndup(arg, klen);
        val = strdup(eq + 1);
        raw = strdup(arg);
    } else {
        key = strdup(arg);
        const char *env_val = getenv(arg);
        val = strdup(env_val ? env_val : "");
        size_t rlen = strlen(key) + 1 + strlen(val) + 1;
        raw = malloc(rlen);
        snprintf(raw, rlen, "%s=%s", key, val);
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
    if (!opts) return;
    free(opts->rootfs);
    free(opts->hostname);
    free(opts->cgroup_name);
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
