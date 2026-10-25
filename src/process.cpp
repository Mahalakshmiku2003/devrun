#include "process.hpp"
#include <unistd.h>

bool start_service(Service& s) {
    int tube[2];
    if (pipe(tube) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        close(tube[0]);
        dup2(tube[1], STDOUT_FILENO);
        dup2(tube[1], STDERR_FILENO);   // errors go to the same tube
        close(tube[1]);
        execlp("sh", "sh", "-c", s.command.c_str(), nullptr);
        _exit(127);
    }

    close(tube[1]);
    s.fd = tube[0];
    s.pid = pid;
    return true;
}
