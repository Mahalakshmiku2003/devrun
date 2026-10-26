#pragma once
#include <deque>
#include <string>
#include <vector>
#include <sys/types.h>

struct Service {
    std::string name;
    std::string command;
    std::vector<std::string> needs;
    int ready_port = 0;
    bool started = false;
    bool ready = false;
    bool failed = false;
    long long started_at_ms = 0;
    int color = 0;
    int fd = -1;
    std::string buf;
    pid_t pid = -1;
    int restarts = 0;
    long long restart_at_ms = 0;
    mutable std::deque<std::string> history;   // recent log lines, for "devrun logs"
};

bool start_service(Service& s);
