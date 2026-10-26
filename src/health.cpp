#include "health.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

bool tcp_check(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // On localhost, connect() succeeds or is refused almost instantly.
    bool ok = connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}
