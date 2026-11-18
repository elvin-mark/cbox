/* Step 6: Resource limits via Linux Cgroups v2 (memory.max & pids.max). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

int main(void) {
    const char *cg = "/sys/fs/cgroup/toy-step6";
    if (mkdir(cg, 0755) < 0 && errno != EEXIST) {
        printf("Skipping cgroup demo (need root/delegation: %s)\n", strerror(errno));
        return 0;
    }

    if (write_str("/sys/fs/cgroup/toy-step6/memory.max", "67108864\n") == 0) {
        printf("Set memory.max to 64MB\n");
    }
    if (write_str("/sys/fs/cgroup/toy-step6/pids.max", "20\n") == 0) {
        printf("Set pids.max to 20\n");
    }

    rmdir(cg);
    return 0;
}
