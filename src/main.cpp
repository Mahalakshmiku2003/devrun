#include "process.hpp"
#include "config.hpp"
#include <cstdio>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

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
        std::vector<pollfd> fds;
        std::vector<int> who;
        for (int i = 0; i < (int)services.size(); i++) {
            if (services[i].fd >= 0) {
                fds.push_back({services[i].fd, POLLIN, 0});
                who.push_back(i);
            }
        }
        if (fds.empty()) break;

        poll(fds.data(), fds.size(), -1);

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
                printf("[%s] exited with code %d\n", s.name.c_str(), WEXITSTATUS(status));
            }
            fflush(stdout);
        }
    }
    return 0;
}
