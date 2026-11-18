/* Step 3: User namespace UID/GID mapping synchronized via a pipe. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

static int child_fn(void *arg) {
    int *pfd = (int *)arg;
    close(pfd[1]);
    char b;
    if (read(pfd[0], &b, 1) != 1) {
        return 1;
    }
    close(pfd[0]);

    printf("[Child] PID=%d, UID=%u, GID=%u\n", getpid(), getuid(), getgid());
    return 0;
}

static void write_file_str(const char *path, const char *str) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, str, __builtin_strlen(str)) < 0) {
            perror("write");
        }
        close(fd);
    }
}

int main(void) {
    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        return 1;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        return 1;
    }

    fflush(stdout);
    int flags = CLONE_NEWPID | CLONE_NEWUSER | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, pfd);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 1;
    }

    close(pfd[0]);
    char path[128], buf[64];
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    snprintf(buf, sizeof(buf), "0 %u 1\n", getuid());
    write_file_str(path, buf);

    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    write_file_str(path, "deny\n");

    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    snprintf(buf, sizeof(buf), "0 %u 1\n", getgid());
    write_file_str(path, buf);

    if (write(pfd[1], "1", 1) != 1) {
        perror("write pipe");
    }
    close(pfd[1]);

    waitpid(pid, NULL, 0);
    free(stack);
    return 0;
}
