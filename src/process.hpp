#pragma once
#include <string>
#include <sys/types.h>

struct Service {
    std::string name;
    std::string command;
    int fd = -1;                  // reading end of its output tube (-1 = not running)
    std::string buf;              // partial line waiting for its newline
    pid_t pid = -1;
    int restarts = 0;             // how many times we've restarted it
    long long restart_at_ms = 0;  // when to restart (0 = nothing scheduled)
};

// Starts the service's command. Returns false if it could not start.
bool start_service(Service& s);
