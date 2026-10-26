#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

struct Service {
    std::string name;
    std::string command;
    std::vector<std::string> needs;   // names of services this one depends on
    bool started = false;             // has it been started at least once?
    int fd = -1;                      // reading end of its output tube (-1 = not running)
    std::string buf;                  // partial line waiting for its newline
    pid_t pid = -1;
    int restarts = 0;
    long long restart_at_ms = 0;
};

bool start_service(Service& s);
