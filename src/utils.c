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

/* Parse sizes like "64M", "512K", "1G", "1048576" */
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
    memset(opts, 0, sizeof(*opts));
}
