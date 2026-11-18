#include "container.h"
#include "json.h"
#include "oci.h"
#include <assert.h>

static int g_tests_run = 0;

#define RUN_TEST(fn)                                                     \
    do {                                                                 \
        g_tests_run++;                                                   \
        fn();                                                            \
        printf("  [PASS] %s\n", #fn);                                    \
    } while (0)

static void test_parse_size_bytes_valid(void) {
    assert(parse_size_bytes("1024") == 1024);
    assert(parse_size_bytes("1K") == 1024);
    assert(parse_size_bytes("64M") == 64ULL * 1024 * 1024);
    assert(parse_size_bytes("64MB") == 64ULL * 1024 * 1024);
    assert(parse_size_bytes("1G") == 1024ULL * 1024 * 1024);
    assert(parse_size_bytes("512k") == 512ULL * 1024);
}

static void test_parse_size_bytes_rejects_invalid(void) {
    assert(parse_size_bytes(NULL) == 0);
    assert(parse_size_bytes("") == 0);
    assert(parse_size_bytes("0") == 0);
    assert(parse_size_bytes("-10M") == 0);
    assert(parse_size_bytes("64X") == 0);
    assert(parse_size_bytes("64Mextra") == 0);
    assert(parse_size_bytes("abc") == 0);
}

static void test_parse_signal_name(void) {
    assert(parse_signal_name("SIGTERM") == 15);
    assert(parse_signal_name("term") == 15);
    assert(parse_signal_name("SIGKILL") == 9);
    assert(parse_signal_name("KILL") == 9);
    assert(parse_signal_name("9") == 9);
    assert(parse_signal_name("15") == 15);
    assert(parse_signal_name("SIGNOPE") == -1);
    assert(parse_signal_name("") == -1);
    assert(parse_signal_name(NULL) == -1);
}

static void test_container_opts_bind(void) {
    container_opts_t opts;
    container_opts_init(&opts);

    assert(container_opts_add_bind(&opts, "/host/dir:/ctr/dir") == 0);
    assert(opts.num_binds == 1);
    assert(strcmp(opts.binds[0].host_path, "/host/dir") == 0);
    assert(strcmp(opts.binds[0].container_path, "/ctr/dir") == 0);
    assert(opts.binds[0].read_only == false);

    assert(container_opts_add_bind(&opts, "/host/ro:/ctr/ro:ro") == 0);
    assert(opts.num_binds == 2);
    assert(opts.binds[1].read_only == true);

    /* Reject malformed binds */
    assert(container_opts_add_bind(&opts, "/no_colon") == -1);
    assert(container_opts_add_bind(&opts, ":/ctr") == -1);
    assert(container_opts_add_bind(&opts, "/host:") == -1);
    assert(container_opts_add_bind(&opts, "/host:/ctr:badopt") == -1);
    assert(opts.num_binds == 2);

    container_opts_free(&opts);
}

static void test_container_opts_env(void) {
    container_opts_t opts;
    container_opts_init(&opts);

    assert(container_opts_add_env(&opts, "APP_ENV=production") == 0);
    assert(opts.num_envs == 1);
    assert(strcmp(opts.envs[0].key, "APP_ENV") == 0);
    assert(strcmp(opts.envs[0].val, "production") == 0);
    assert(strcmp(opts.envs[0].raw, "APP_ENV=production") == 0);

    assert(container_opts_add_env(&opts, "EMPTY=") == 0);
    assert(strcmp(opts.envs[1].val, "") == 0);

    assert(container_opts_add_env(&opts, "=NOKEY") == -1);
    assert(container_opts_add_env(&opts, "") == -1);

    container_opts_free(&opts);
}

static void test_json_parser_oci_snippet(void) {
    const char *sample =
        "{"
        "  \"ociVersion\": \"1.0.2\","
        "  \"hostname\": \"k8s-pod\","
        "  \"root\": {\"path\": \"rootfs\", \"readonly\": true},"
        "  \"process\": {"
        "    \"args\": [\"/bin/sh\", \"-c\", \"echo \\\"hi\\\"\"],"
        "    \"noNewPrivileges\": true"
        "  },"
        "  \"linux\": {"
        "    \"resources\": {\"memory\": {\"limit\": 67108864}, \"pids\": {\"limit\": 32}}"
        "  }"
        "}";

    json_value_t *root = json_parse(sample);
    assert(root != NULL);
    assert(strcmp(json_as_string(json_obj_get(root, "hostname")), "k8s-pod") == 0);

    const json_value_t *r = json_obj_get(root, "root");
    assert(json_as_bool(json_obj_get(r, "readonly"), false) == true);

    const json_value_t *proc = json_obj_get(root, "process");
    const json_value_t *args = json_obj_get(proc, "args");
    assert(args && args->type == JSON_ARRAY && args->u.array.count == 3);
    assert(strcmp(json_as_string(args->u.array.items[2]), "echo \"hi\"") == 0);

    const json_value_t *lin = json_obj_get(root, "linux");
    const json_value_t *res = json_obj_get(lin, "resources");
    assert(json_as_int64(json_obj_get(json_obj_get(res, "memory"), "limit"), 0) == 67108864);
    assert(json_as_int64(json_obj_get(json_obj_get(res, "pids"), "limit"), 0) == 32);

    json_free(root);

    /* Malformed inputs */
    assert(json_parse("{bad}") == NULL);
    assert(json_parse("[1, 2,]") == NULL);
    assert(json_parse("{\"a\": 1} trailing") == NULL);
    assert(json_parse("") == NULL);
}

int main(void) {
    log_set_quiet(true);
    printf("=== Running Unit Tests ===\n");
    RUN_TEST(test_parse_size_bytes_valid);
    RUN_TEST(test_parse_size_bytes_rejects_invalid);
    RUN_TEST(test_parse_signal_name);
    RUN_TEST(test_container_opts_bind);
    RUN_TEST(test_container_opts_env);
    RUN_TEST(test_json_parser_oci_snippet);
    printf("=== %d/%d Unit Tests Passed ===\n", g_tests_run, g_tests_run);
    return 0;
}
