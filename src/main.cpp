#include "process.hpp"
#include "config.hpp"
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

static const int MAX_RESTARTS = 5;
static const int GRACE_MS = 3000;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }   // only set a flag; do the real work in the loop

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Stop everything: polite SIGTERM first, SIGKILL for anything still alive after GRACE_MS.
static void shutdown_all(std::vector<Service>& services) {
    printf("\nshutting down...\n");
    fflush(stdout);

    for (auto& s : services) {
        s.restart_at_ms = 0;                       // cancel pending restarts
        if (s.pid > 0) kill(-s.pid, SIGTERM);      // negative pid = the whole process group
    }

    long long deadline = now_ms() + GRACE_MS;
    while (now_ms() < deadline) {
        bool any = false;
        for (auto& s : services) {
            if (s.pid <= 0) continue;
            int status;
            pid_t r = waitpid(s.pid, &status, WNOHANG);
            if (r == s.pid) {
                printf("[%s] stopped\n", s.name.c_str());
                s.pid = -1;
            } else {
                any = true;
            }
        }
        if (!any) break;
        usleep(50 * 1000);
    }

    for (auto& s : services) {
        if (s.pid <= 0) continue;
        printf("[%s] did not stop, sending SIGKILL\n", s.name.c_str());
        kill(-s.pid, SIGKILL);
        waitpid(s.pid, nullptr, 0);
        s.pid = -1;
    }

    for (auto& s : services) {
        if (s.fd >= 0) { close(s.fd); s.fd = -1; }
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    std::vector<Service> services;
    std::string error;
    const char* path = (argc > 1) ? argv[1] : "devrun.conf";
    if (!load_config(path, services, error)) {
        fprintf(stderr, "config error: %s\n", error.c_str());
        return 1;
    }

    // Catch Ctrl+C (SIGINT) and SIGTERM. No SA_RESTART, so poll() wakes up when a signal arrives.
    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    for (auto& s : services) {
        if (!start_service(s)) printf("[%s] failed to start\n", s.name.c_str());
    }

    while (!g_stop) {
        long long now = now_ms();
        int timeout = -1;
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
        if (fds.empty() && !pending) break;

        int r = poll(fds.data(), fds.size(), timeout);
        if (r < 0) {
            if (errno == EINTR) continue;   // a signal woke us; the loop condition checks g_stop
            break;
        }

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
                s.pid = -1;
                int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                             : 128 + WTERMSIG(status);
                printf("[%s] exited with code %d\n", s.name.c_str(), code);

                if (code != 0) {
                    if (s.restarts < MAX_RESTARTS) {
                        long long delay = 1000LL << s.restarts;
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

    if (g_stop) shutdown_all(services);
    return 0;
}
