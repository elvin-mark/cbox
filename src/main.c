#include "container.h"
#include "oci.h"
#include <getopt.h>

static void print_usage(const char *prog_name) {
    printf("Usage (Educational CLI):\n");
    printf("  %s run [options] [--] <command> [args...]\n", prog_name);
    printf("  %s create-rootfs <directory>\n", prog_name);
    printf("  %s help\n\n", prog_name);
    printf("Usage (OCI Runtime Mode for containerd / Kubernetes):\n");
    printf("  %s [--root <dir>] create --bundle <dir> [--pid-file <path>] <id>\n", prog_name);
    printf("  %s [--root <dir>] start <id>\n", prog_name);
    printf("  %s [--root <dir>] state <id>\n", prog_name);
    printf("  %s [--root <dir>] kill <id> [signal]\n", prog_name);
    printf("  %s [--root <dir>] delete [--force] <id>\n", prog_name);
    printf("  %s [--root <dir>] run --bundle <dir> <id>\n\n", prog_name);
    printf("Run Options:\n");
    printf("  -r, --rootfs <path>          Path to root filesystem (pivot_root/chroot)\n");
    printf("      --ro                     Remount rootfs read-only\n");
    printf("  -n, --hostname <name>        Hostname inside container (default: toy-container)\n");
    printf("      --cwd <dir>              Working directory inside container\n");
    printf("  -m, --memory <size>          Memory limit (e.g., 64M, 512M, 1G)\n");
    printf("  -p, --pids <count>           Maximum number of processes in container\n");
    printf("      --cpu <quota_us>         CPU quota in microseconds per 100ms period\n");
    printf("  -v, --volume <host>:<target> Bind mount host directory or file (requires -r)\n");
    printf("  -e, --env <KEY=VAL>          Set environment variable inside container\n");
    printf("      --init                   Run PID 1 init zombie reaper inside container\n");
    printf("      --no-new-privs           Set PR_SET_NO_NEW_PRIVS\n");
    printf("      --drop-caps              Drop dangerous Linux capabilities\n");
    printf("  -V, --verbose                Enable detailed debug logging\n");
    printf("      --no-net                 Do not isolate network namespace\n");
    printf("  -h, --help                   Display this help message\n");
}

static bool parse_strict_long(const char *str, long *out) {
    if (!str || str[0] == '\0') {
        return false;
    }
    char *endptr = NULL;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val <= 0) {
        return false;
    }
    *out = val;
    return true;
}

static int handle_oci_run_or_create(const char *state_root, int argc, char *argv[],
                                    int start_idx, bool is_create) {
    const char *bundle = ".";
    const char *pid_file = NULL;

    static struct option oci_opts[] = {
        {"bundle",       required_argument, 0, 'b'},
        {"pid-file",     required_argument, 0, 'P'},
        {"console-socket", required_argument, 0, 'c'},
        {"no-pivot",     no_argument,       0, 1001},
        {0, 0, 0, 0}
    };

    optind = start_idx;
    int opt;
    while ((opt = getopt_long(argc, argv, "b:P:c:", oci_opts, NULL)) != -1) {
        switch (opt) {
            case 'b':
                bundle = optarg;
                break;
            case 'P':
                pid_file = optarg;
                break;
            case 'c':
            case 1001:
                break;
            default:
                return 1;
        }
    }

    if (optind >= argc) {
        log_error("Container ID required");
        return 1;
    }
    const char *container_id = argv[optind];
    if (is_create) {
        return oci_cmd_create(state_root, container_id, bundle, pid_file);
    }
    return oci_cmd_run(state_root, container_id, bundle, pid_file);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    char default_root_buf[512];
    const char *state_root = oci_default_state_root(default_root_buf, sizeof(default_root_buf));

    /* Parse optional global flags before subcommand (OCI style: --root, --systemd-cgroup) */
    int cmd_idx = 1;
    while (cmd_idx < argc && argv[cmd_idx][0] == '-') {
        if (strcmp(argv[cmd_idx], "--help") == 0 || strcmp(argv[cmd_idx], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[cmd_idx], "--root") == 0 && cmd_idx + 1 < argc) {
            state_root = argv[cmd_idx + 1];
            cmd_idx += 2;
        } else if (strncmp(argv[cmd_idx], "--root=", 7) == 0) {
            state_root = argv[cmd_idx] + 7;
            cmd_idx += 1;
        } else if (strcmp(argv[cmd_idx], "--systemd-cgroup") == 0) {
            cmd_idx += 1;
        } else if ((strcmp(argv[cmd_idx], "--log") == 0 ||
                    strcmp(argv[cmd_idx], "--log-format") == 0) &&
                   cmd_idx + 1 < argc) {
            cmd_idx += 2;
        } else {
            break;
        }
    }

    if (cmd_idx >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char *subcmd = argv[cmd_idx];

    if (strcmp(subcmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(subcmd, "create-rootfs") == 0) {
        if (cmd_idx + 1 >= argc) {
            log_error("Target directory required for create-rootfs");
            return 1;
        }
        return create_minimal_rootfs(argv[cmd_idx + 1]) == 0 ? 0 : 1;
    }

    /* OCI lifecycle subcommands */
    if (strcmp(subcmd, "state") == 0) {
        if (cmd_idx + 1 >= argc) {
            log_error("Container ID required for state");
            return 1;
        }
        return oci_cmd_state(state_root, argv[cmd_idx + 1]);
    }

    if (strcmp(subcmd, "create") == 0) {
        return handle_oci_run_or_create(state_root, argc, argv, cmd_idx + 1, true);
    }

    if (strcmp(subcmd, "start") == 0) {
        if (cmd_idx + 1 >= argc) {
            log_error("Container ID required for start");
            return 1;
        }
        return oci_cmd_start(state_root, argv[cmd_idx + 1]);
    }

    if (strcmp(subcmd, "kill") == 0) {
        if (cmd_idx + 1 >= argc) {
            log_error("Container ID required for kill");
            return 1;
        }
        const char *sig = (cmd_idx + 2 < argc) ? argv[cmd_idx + 2] : NULL;
        return oci_cmd_kill(state_root, argv[cmd_idx + 1], sig);
    }

    if (strcmp(subcmd, "delete") == 0) {
        bool force = false;
        const char *id = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                force = true;
            } else {
                id = argv[i];
            }
        }
        if (!id) {
            log_error("Container ID required for delete");
            return 1;
        }
        return oci_cmd_delete(state_root, id, force);
    }

    if (strcmp(subcmd, "run") != 0) {
        log_error("Unknown command '%s'", subcmd);
        print_usage(argv[0]);
        return 1;
    }

    /* Check if 'run' is invoked with OCI --bundle / -b */
    for (int i = cmd_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            break;
        }
        if (strcmp(argv[i], "--bundle") == 0 || strcmp(argv[i], "-b") == 0 ||
            strncmp(argv[i], "--bundle=", 9) == 0) {
            return handle_oci_run_or_create(state_root, argc, argv, cmd_idx + 1, false);
        }
    }

    container_opts_t opts;
    container_opts_init(&opts);

    static struct option long_options[] = {
        {"rootfs",       required_argument, 0, 'r'},
        {"ro",           no_argument,       0, 1001},
        {"hostname",     required_argument, 0, 'n'},
        {"cwd",          required_argument, 0, 1002},
        {"memory",       required_argument, 0, 'm'},
        {"pids",         required_argument, 0, 'p'},
        {"cpu",          required_argument, 0, 1003},
        {"volume",       required_argument, 0, 'v'},
        {"env",          required_argument, 0, 'e'},
        {"init",         no_argument,       0, 1004},
        {"no-new-privs", no_argument,       0, 1005},
        {"drop-caps",    no_argument,       0, 1006},
        {"verbose",      no_argument,       0, 'V'},
        {"no-net",       no_argument,       0, 1000},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = cmd_idx + 1;

    while ((opt = getopt_long(argc, argv, "+r:n:m:p:v:e:Vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'r':
                free(opts.rootfs);
                opts.rootfs = strdup(optarg);
                break;
            case 1001:
                opts.rootfs_readonly = true;
                break;
            case 'n':
                free(opts.hostname);
                opts.hostname = strdup(optarg);
                break;
            case 1002:
                free(opts.cwd);
                opts.cwd = strdup(optarg);
                break;
            case 'm':
                opts.mem_max_bytes = parse_size_bytes(optarg);
                if (opts.mem_max_bytes == 0) {
                    log_warn("Invalid memory limit '%s', ignoring", optarg);
                }
                break;
            case 'p':
                if (!parse_strict_long(optarg, &opts.pids_max)) {
                    log_warn("Invalid pids limit '%s', ignoring", optarg);
                    opts.pids_max = 0;
                }
                break;
            case 1003:
                if (!parse_strict_long(optarg, &opts.cpu_quota_us)) {
                    log_warn("Invalid CPU quota '%s', ignoring", optarg);
                    opts.cpu_quota_us = 0;
                }
                break;
            case 'v':
                if (container_opts_add_bind(&opts, optarg) < 0) {
                    log_warn("Failed to parse volume bind '%s'", optarg);
                    container_opts_free(&opts);
                    return 1;
                }
                break;
            case 'e':
                if (container_opts_add_env(&opts, optarg) < 0) {
                    log_warn("Failed to parse environment variable '%s'", optarg);
                    container_opts_free(&opts);
                    return 1;
                }
                break;
            case 1004:
                opts.init_reaper = true;
                break;
            case 1005:
                opts.no_new_privs = true;
                break;
            case 1006:
                opts.drop_caps = true;
                break;
            case 'V':
                opts.verbose = true;
                break;
            case 1000:
                opts.isolate_net = false;
                opts.namespaces[NS_IDX_NET].enabled = false;
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

    int cmd_argc = argc - optind;
    opts.cmd_argv = calloc((size_t)cmd_argc + 1, sizeof(char *));
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
