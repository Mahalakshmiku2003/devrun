#pragma once
#include <string>

// Server side
int ipc_listen(const std::string& path, std::string& error);  // returns listening fd, or -1
std::string ipc_read_line(int fd, int timeout_ms);            // read one request line
void ipc_reply(int fd, const std::string& text);              // send text to a connection

// Client side
bool ipc_alive(const std::string& path);                      // is a devrun listening there?
bool ipc_request(const std::string& path, const std::string& cmd, std::string& reply);
