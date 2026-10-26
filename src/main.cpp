#include "process.hpp"
#include "config.hpp"
#include "health.hpp"
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

static const int MAX_RESTARTS = 5;
static const int GRACE_MS = 3000;
static const int READY_TIMEOUT_MS = 15000;
static const int CHECK_INTERVAL_MS = 200;
static const int STABLE_MS = 60000;   // ran this long before crashing = counter resets

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static bool g_color = false;
static int g_width = 6;
static const char* PALETTE[] = {"36", "32", "33", "35", "34", "91", "92", "96"};

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Prints one log line: "HH:MM:SS name | message", colored per service.
static void emit(const char* name, int color, const char* msg) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    if (g_color) {
        const char* c = (color >= 0) ? PALETTE[color % 8] : "90";
        printf("\033[90m%s\033[0m \033[%sm%-*s |\033[0m %s\n", ts, c, g_width, name, msg);
    } else {
        printf("%s %-*s | %s\n", ts, g_width, name, msg);
    }
    fflush(stdout);
}

// Message from a service (or about a service).
static void say(const Service& s, const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit(s.name.c_str(), s.color, msg);
}

// Message from devrun itself.
static void note(const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit("devrun", -1, msg);
}

static void usage() {
    printf(
        "devrun - run several dev services from one config file\n"
        "\n"
        "usage: devrun [options]\n"
        "  -f, --file FILE   config file to use (default: devrun.conf)\n"
        "      --no-color    turn off colors\n"
        "  -h, --help        show this help\n"
        "\n"
        "config format:\n"
        "  [name]\n"
        "  cmd   = command to run\n"
        "  needs = other, services      (optional)\n"
        "  ready = port 5432            (optional)\n"
        "\n"
        "Press Ctrl+C to stop all services.\n");
}

static Service* find_service(std::vector<Service>& v, const std::string& name) {
    for (auto& s : v) if (s.name == name) return &s;
    return nullptr;
}

static void mark_started(Service& s) {
    s.started = true;
    s.started_at_ms = now_ms();
    s.ready = (s.ready_port == 0);
}

static void shutdown_all(std::vector<Service>& services) {
    note("shutting down...");

    for (auto& s : services) {
        s.restart_at_ms = 0;
        if (s.pid > 0) kill(-s.pid, SIGTERM);
    }

    long long deadline = now_ms() + GRACE_MS;
    while (now_ms() < deadline) {
        bool any = false;
        for (auto& s : services) {
            if (s.pid <= 0) continue;
            int status;
            pid_t r = waitpid(s.pid, &status, WNOHANG);
            if (r == s.pid) {
                say(s, "stopped");
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
        say(s, "did not stop, sending SIGKILL");
        kill(-s.pid, SIGKILL);
        waitpid(s.pid, nullptr, 0);
        s.pid = -1;
    }

    for (auto& s : services) {
        if (s.fd >= 0) { close(s.fd); s.fd = -1; }
    }
}

int main(int argc, char** argv) {
    const char* path = "devrun.conf";
    bool no_color = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--no-color") {
            no_color = true;
        } else if (a == "-f" || a == "--file") {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a file name\n", a.c_str());
                return 1;
            }
            path = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            path = argv[i];           // plain file name also works
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", a.c_str());
            return 1;
        }
    }

    g_color = isatty(STDOUT_FILENO) && !no_color && !getenv("NO_COLOR");

    std::vector<Service> services;
    std::string error;
    if (!load_config(path, services, error)) {
        fprintf(stderr, "config error: %s\n", error.c_str());
        return 1;
    }

    for (size_t i = 0; i < services.size(); i++) {
        services[i].color = (int)i;
        if ((int)services[i].name.size() > g_width) g_width = (int)services[i].name.size();
    }

    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    {
        std::string order;
        for (auto& s : services) order += " " + s.name;
        note("start order:%s", order.c_str());
    }

    while (!g_stop) {
        // A. Start services whose dependencies are ready; check readiness of the rest.
        for (auto& s : services) {
            if (s.failed) continue;

            if (!s.started) {
                bool all_ready = true;
                bool dep_failed = false;
                for (auto& n : s.needs) {
                    Service* d = find_service(services, n);
                    if (d->failed) dep_failed = true;
                    else if (!d->ready) all_ready = false;
                }
                if (dep_failed) {
                    say(s, "not started: a dependency failed");
                    s.failed = true;
                } else if (all_ready) {
                    if (start_service(s)) mark_started(s);
                    else { say(s, "failed to start"); s.failed = true; }
                }
            } else if (!s.ready) {
                if (tcp_check(s.ready_port)) {
                    s.ready = true;
                    say(s, "ready (port %d is accepting connections)", s.ready_port);
                } else if (now_ms() - s.started_at_ms > READY_TIMEOUT_MS) {
                    say(s, "not ready after %d s, giving up on it", READY_TIMEOUT_MS / 1000);
                    s.failed = true;
                }
            }
        }

        // B. Decide how long poll may sleep.
        long long now = now_ms();
        int timeout = -1;
        bool pending = false;
        bool waiting = false;
        for (auto& s : services) {
            if (s.restart_at_ms > 0) {
                pending = true;
                long long wait = s.restart_at_ms - now;
                if (wait < 0) wait = 0;
                if (timeout < 0 || wait < timeout) timeout = (int)wait;
            }
            if (!s.failed && (!s.started || !s.ready)) waiting = true;
        }
        if (waiting && (timeout < 0 || timeout > CHECK_INTERVAL_MS)) timeout = CHECK_INTERVAL_MS;

        std::vector<pollfd> fds;
        std::vector<int> who;
        for (int i = 0; i < (int)services.size(); i++) {
            if (services[i].fd >= 0) {
                fds.push_back({services[i].fd, POLLIN, 0});
                who.push_back(i);
            }
        }
        if (fds.empty() && !pending && !waiting) break;

        int r = poll(fds.data(), fds.size(), timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // C. Read output / detect exits.
        for (size_t k = 0; k < fds.size(); k++) {
            if (!fds[k].revents) continue;
            Service& s = services[who[k]];

            char tmp[512];
            ssize_t n = read(s.fd, tmp, sizeof(tmp));
            if (n > 0) {
                s.buf.append(tmp, n);
                size_t pos;
                while ((pos = s.buf.find('\n')) != std::string::npos) {
                    say(s, "%s", s.buf.substr(0, pos).c_str());
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
                say(s, "exited with code %d", code);

                if (code != 0) {
                    if (s.restarts > 0 && now_ms() - s.started_at_ms >= STABLE_MS) {
                        s.restarts = 0;
                        say(s, "ran stably for %d s, restart counter reset", STABLE_MS / 1000);
                    }
                    if (s.restarts < MAX_RESTARTS) {
                        long long delay = 1000LL << s.restarts;
                        s.restarts++;
                        s.restart_at_ms = now_ms() + delay;
                        say(s, "crashed, restarting in %lld s (attempt %d of %d)",
                            delay / 1000, s.restarts, MAX_RESTARTS);
                    } else {
                        say(s, "crashed too many times, giving up");
                        s.failed = true;
                    }
                }
            }
        }

        // D. Restart services whose time has come.
        now = now_ms();
        for (auto& s : services) {
            if (s.restart_at_ms > 0 && now >= s.restart_at_ms) {
                s.restart_at_ms = 0;
                s.buf.clear();
                say(s, "restarting now");
                if (start_service(s)) mark_started(s);
                else say(s, "failed to start");
            }
        }
    }

    if (g_stop) shutdown_all(services);
    return 0;
}
