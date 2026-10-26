#include "ipc.hpp"
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool make_addr(const std::string& path, sockaddr_un& addr) {
    if (path.size() >= sizeof(addr.sun_path)) return false;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

static int connect_to(const std::string& path) {
    sockaddr_un addr;
    if (!make_addr(path, addr)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool ipc_alive(const std::string& path) {
    int fd = connect_to(path);
    if (fd < 0) return false;
    close(fd);
    return true;
}

int ipc_listen(const std::string& path, std::string& error) {
    if (ipc_alive(path)) {
        error = "devrun is already running in this folder";
        return -1;
    }
    unlink(path.c_str());   // a leftover file from a crashed run would block bind()

    sockaddr_un addr;
    if (!make_addr(path, addr)) {
        error = "socket path too long";
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = "cannot create socket";
        return -1;
    }
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        error = "cannot listen on " + path + ": " + strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

std::string ipc_read_line(int fd, int timeout_ms) {
    std::string line;
    char c;
    while (line.size() < 256) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) break;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0 || c == '\n') break;
        line += c;
    }
    return line;
}

void ipc_reply(int fd, const std::string& text) {
    size_t off = 0;
    while (off < text.size()) {
        // MSG_NOSIGNAL: if the client has gone away, return an error instead of killing us
        ssize_t n = send(fd, text.data() + off, text.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
}

bool ipc_request(const std::string& path, const std::string& cmd, std::string& reply) {
    int fd = connect_to(path);
    if (fd < 0) return false;
    ipc_reply(fd, cmd + "\n");
    reply.clear();
    char buf[1024];
    while (true) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 10000) <= 0) break;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        reply.append(buf, (size_t)n);
    }
    close(fd);
    return true;
}
