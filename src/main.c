#include "container.h"
#include <getopt.h>

static void print_usage(const char *prog_name) {
    printf("Usage:\n");
    printf("  %s run [options] [--] <command> [args...]\n", prog_name);
    printf("  %s create-rootfs <directory>\n", prog_name);
    printf("  %s help\n\n", prog_name);
    printf("Options:\n");
    printf("  -r, --rootfs <path>     Path to root filesystem (chroot/pivot_root)\n");
    printf("  -n, --hostname <name>   Hostname inside container (default: toy-container)\n");
    printf("  -m, --memory <size>     Memory limit (e.g., 64M, 512M, 1G)\n");
    printf("  -p, --pids <count>      Maximum number of processes in container\n");
    printf("  -v, --verbose           Enable detailed debug logging\n");
    printf("  --no-net                Do not isolate network namespace\n");
    printf("  -h, --help              Display this help message\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "create-rootfs") == 0) {
        if (argc < 3) {
            log_error("Target directory required for create-rootfs");
            printf("Usage: %s create-rootfs <directory>\n", argv[0]);
            return 1;
        }
        return create_minimal_rootfs(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "run") != 0) {
        log_error("Unknown command '%s'", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    container_opts_t opts;
    container_opts_init(&opts);

    static struct option long_options[] = {
        {"rootfs",   required_argument, 0, 'r'},
        {"hostname", required_argument, 0, 'n'},
        {"memory",   required_argument, 0, 'm'},
        {"pids",     required_argument, 0, 'p'},
        {"verbose",  no_argument,       0, 'v'},
        {"no-net",   no_argument,       0, 1000},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    /* Shift past "run" command */
    int opt;
    optind = 2;

    while ((opt = getopt_long(argc, argv, "+r:n:m:p:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'r':
                opts.rootfs = strdup(optarg);
                break;
            case 'n':
                free(opts.hostname);
                opts.hostname = strdup(optarg);
                break;
            case 'm':
                opts.mem_max_bytes = parse_size_bytes(optarg);
                if (opts.mem_max_bytes == 0) {
                    log_warn("Invalid memory limit '%s', ignoring", optarg);
                }
                break;
            case 'p':
                opts.pids_max = strtol(optarg, NULL, 10);
                if (opts.pids_max <= 0) {
                    log_warn("Invalid pids limit '%s', ignoring", optarg);
                    opts.pids_max = 0;
                }
                break;
            case 'v':
                opts.verbose = true;
                break;
            case 1000:
                opts.isolate_net = false;
                break;
            case 'h':
                print_usage(argv[0]);
                container_opts_free(&opts);
                return 0;
            default:
                print_usage(argv[0]);
                container_opts_free(&opts);
                return 1;
        }
    }

    if (optind >= argc) {
        log_error("No command specified to run inside container!");
        print_usage(argv[0]);
        container_opts_free(&opts);
        return 1;
    }

    /* Collect remaining arguments for container command */
    int cmd_argc = argc - optind;
    opts.cmd_argv = calloc(cmd_argc + 1, sizeof(char *));
    if (!opts.cmd_argv) {
        log_error("Memory allocation failed");
        container_opts_free(&opts);
        return 1;
    }

    for (int i = 0; i < cmd_argc; i++) {
        opts.cmd_argv[i] = strdup(argv[optind + i]);
    }
    opts.cmd_argv[cmd_argc] = NULL;

    int res = container_run(&opts);

    container_opts_free(&opts);
    return res >= 0 ? res : 1;
}
