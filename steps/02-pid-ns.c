/* Step 2: Process ID isolation with CLONE_NEWPID (requires root or CLONE_NEWUSER). */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

static int child_fn(void *arg) {
    (void)arg;
    printf("[Child] Inside PID namespace: PID=%d\n", getpid());
    return 0;
}

int main(void) {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return 1;
    }

    fflush(stdout);
    int flags = CLONE_NEWPID | CLONE_NEWUSER | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, NULL);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 1;
    }

    printf("[Parent] Spawned child with Host PID=%d\n", pid);
    waitpid(pid, NULL, 0);
    free(stack);
    return 0;
}
