#include "process.hpp"
#include "config.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

static const int MAX_RESTARTS = 5;

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::vector<Service> services;
    std::string error;
    const char* path = (argc > 1) ? argv[1] : "devrun.conf";
    if (!load_config(path, services, error)) {
        fprintf(stderr, "config error: %s\n", error.c_str());
        return 1;
    }

    for (auto& s : services) {
        if (!start_service(s)) printf("[%s] failed to start\n", s.name.c_str());
    }

    while (true) {
        // Work out how long poll may sleep: until the next scheduled restart
        long long now = now_ms();
        int timeout = -1;          // -1 = sleep until data arrives
        bool pending = false;
        for (auto& s : services) {
            if (s.restart_at_ms > 0) {
                pending = true;
                long long wait = s.restart_at_ms - now;
                if (wait < 0) wait = 0;
                if (timeout < 0 || wait < timeout) timeout = (int)wait;
            }
        }

        std::vector<pollfd> fds;
        std::vector<int> who;
        for (int i = 0; i < (int)services.size(); i++) {
            if (services[i].fd >= 0) {
                fds.push_back({services[i].fd, POLLIN, 0});
                who.push_back(i);
            }
        }
        if (fds.empty() && !pending) break;   // nothing running, nothing to restart

        poll(fds.data(), fds.size(), timeout);

        // 1. Read output / detect exits
        for (size_t k = 0; k < fds.size(); k++) {
            if (!fds[k].revents) continue;
            Service& s = services[who[k]];

            char tmp[512];
            ssize_t n = read(s.fd, tmp, sizeof(tmp));
            if (n > 0) {
                s.buf.append(tmp, n);
                size_t pos;
                while ((pos = s.buf.find('\n')) != std::string::npos) {
                    printf("[%s] %s\n", s.name.c_str(), s.buf.substr(0, pos).c_str());
                    s.buf.erase(0, pos + 1);
                }
            } else {
                close(s.fd);
                s.fd = -1;
                int status;
                waitpid(s.pid, &status, 0);
                int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                             : 128 + WTERMSIG(status);
                printf("[%s] exited with code %d\n", s.name.c_str(), code);

                if (code != 0) {
                    if (s.restarts < MAX_RESTARTS) {
                        long long delay = 1000LL << s.restarts;   // 1s, 2s, 4s...
                        s.restarts++;
                        s.restart_at_ms = now_ms() + delay;
                        printf("[%s] crashed, restarting in %lld s (attempt %d of %d)\n",
                               s.name.c_str(), delay / 1000, s.restarts, MAX_RESTARTS);
                    } else {
                        printf("[%s] crashed too many times, giving up\n", s.name.c_str());
                    }
                }
            }
            fflush(stdout);
        }

        // 2. Start any service whose restart time has arrived
        now = now_ms();
        for (auto& s : services) {
            if (s.restart_at_ms > 0 && now >= s.restart_at_ms) {
                s.restart_at_ms = 0;
                s.buf.clear();
                printf("[%s] restarting now\n", s.name.c_str());
                if (!start_service(s)) printf("[%s] failed to start\n", s.name.c_str());
                fflush(stdout);
            }
        }
    }
    return 0;
}
