/* Step 4: Mount namespace isolation (MS_PRIVATE) and mounting a fresh /proc. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

static int child_fn(void *arg) {
    (void)arg;
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "[Child] MS_PRIVATE mount skipped (%s)\n", strerror(errno));
    }
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
        fprintf(stderr, "[Child] /proc mount skipped (%s)\n", strerror(errno));
    } else {
        printf("[Child] Mounted fresh /proc!\n");
    }
    printf("[Child] Container PID=%d\n", getpid());
    return 0;
}

int main(void) {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        return 1;
    }

    fflush(stdout);
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, NULL);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 1;
    }

    waitpid(pid, NULL, 0);
    free(stack);
    return 0;
}
