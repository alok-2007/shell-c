void executePipe(Command *left, Command *right) {

  int fd[2];

  pipe(fd);

  pid_t pid1 = fork();

  if (pid1 == 0) {

    dup2(fd[1], STDOUT_FILENO);

    close(fd[0]);
    close(fd[1]);

    left->argv[left->argc] = NULL;

    char *path = isExec(left->argv[0]);

    execv(path, left->argv);

    exit(1);
  }

  pid_t pid2 = fork();

  if (pid2 == 0) {

    dup2(fd[0], STDIN_FILENO);

    close(fd[1]);
    close(fd[0]);

    right->argv[right->argc] = NULL;

    char *path = isExec(right->argv[0]);

    execv(path, right->argv);

    exit(1);
  }

  close(fd[0]);
  close(fd[1]);

  waitpid(pid1, NULL, 0);
  waitpid(pid2, NULL, 0);
}