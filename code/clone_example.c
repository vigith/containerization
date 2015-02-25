#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>

#define STACKSIZE (1024*1024)

/* the flags */
static int clone_flags = SIGCHLD;

/* fn_child_exec is the func that will be executed by clone
   and when this function returns, the child process will be
   terminated */
static int fn_child_exec(void *arg) {
  char * const cmd[] = { "/bin/bash", NULL};
  fprintf(stderr, "Child Pid: [%d] Invoking Command [%s] \n", getpid(), cmd[0]);
  if (execv(cmd[0], cmd) != 0) {
    fprintf(stderr, "Failed to Run [%s] (Error: %s)\n", cmd[0], strerror(errno));
    exit(-1);
  }
  /* exec never returns */
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv) {
  char *child_stack = (char *)malloc(STACKSIZE*sizeof(char));

  /* create a new process, the function fn_child_exec will be called */
  pid_t pid = clone(fn_child_exec, child_stack + STACKSIZE, clone_flags, NULL);
    
  if (pid < 0) {
    fprintf(stderr, "clone failed (Reason: %s)\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  /* lets wait on our child process here before we, the parent, exits */
  if (waitpid(pid, NULL, 0) == -1) {
    fprintf(stderr, "'waitpid' for pid [%d] failed (Reason: %s)\n", pid, strerror(errno));
    exit(EXIT_FAILURE);
  }

  return 0;
}
