#include "config.hpp"
#include <fstream>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool load_config(const std::string& path, std::vector<Service>& out, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }

    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        lineno++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            error = "line " + std::to_string(lineno) + ": expected 'name: command'";
            return false;
        }

        Service s;
        s.name = trim(line.substr(0, colon));
        s.command = trim(line.substr(colon + 1));
        if (s.name.empty() || s.command.empty()) {
            error = "line " + std::to_string(lineno) + ": name or command is empty";
            return false;
        }
        out.push_back(s);
    }
    return true;
}
