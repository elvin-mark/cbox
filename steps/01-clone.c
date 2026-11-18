/* Step 1: Spawning a process with clone() and a manual stack. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

static int child_fn(void *arg) {
    (void)arg;
    printf("[Child] Hello from clone! PID=%d, PPID=%d\n", getpid(), getppid());
    return 0;
}

int main(void) {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return 1;
    }
    char *stack_top = stack + STACK_SIZE;

    printf("[Parent] Host PID=%d\n", getpid());
    fflush(stdout);

    pid_t pid = clone(child_fn, stack_top, SIGCHLD, NULL);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 1;
    }

    waitpid(pid, NULL, 0);
    free(stack);
    return 0;
}
