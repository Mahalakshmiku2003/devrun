#include "config.hpp"
#include <fstream>
#include <map>
#include <set>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string item = trim(s.substr(start, comma - start));
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
    return out;
}

static bool order_services(std::vector<Service>& svcs, std::string& error) {
    std::set<std::string> names;
    for (auto& s : svcs) {
        if (!names.insert(s.name).second) {
            error = "duplicate service name '" + s.name + "'";
            return false;
        }
    }
    for (auto& s : svcs) {
        for (auto& n : s.needs) {
            if (!names.count(n)) {
                error = "service '" + s.name + "' needs unknown service '" + n + "'";
                return false;
            }
        }
    }

    std::vector<Service> ordered;
    std::set<std::string> placed;
    std::vector<bool> done(svcs.size(), false);

    while (ordered.size() < svcs.size()) {
        bool progress = false;
        for (size_t i = 0; i < svcs.size(); i++) {
            if (done[i]) continue;
            bool ready = true;
            for (auto& n : svcs[i].needs) {
                if (!placed.count(n)) ready = false;
            }
            if (ready) {
                ordered.push_back(svcs[i]);
                placed.insert(svcs[i].name);
                done[i] = true;
                progress = true;
            }
        }
        if (!progress) {
            error = "circular dependency among:";
            for (size_t i = 0; i < svcs.size(); i++) {
                if (!done[i]) error += " " + svcs[i].name;
            }
            return false;
        }
    }
    svcs = ordered;
    return true;
}

bool load_config(const std::string& path, std::vector<Service>& out, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }

    std::vector<Service> svcs;
    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        lineno++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::string where = "line " + std::to_string(lineno) + ": ";

        if (line[0] == '[') {
            if (line.back() != ']' || line.size() < 3) {
                error = where + "expected [name]";
                return false;
            }
            Service s;
            s.name = trim(line.substr(1, line.size() - 2));
            svcs.push_back(s);
            continue;
        }

        if (svcs.empty()) {
            error = where + "expected a [name] section first";
            return false;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = where + "expected 'key = value'";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        if (key == "cmd") {
            svcs.back().command = value;
        } else if (key == "needs") {
            svcs.back().needs = split_commas(value);
        } else if (key == "ready") {
            if (value.rfind("port ", 0) != 0) {
                error = where + "expected 'ready = port NUMBER'";
                return false;
            }
            int port = 0;
            try { port = std::stoi(trim(value.substr(5))); } catch (...) { port = 0; }
            if (port < 1 || port > 65535) {
                error = where + "invalid port number";
                return false;
            }
            svcs.back().ready_port = port;
        } else {
            error = where + "unknown key '" + key + "' (use cmd, needs or ready)";
            return false;
        }
    }

    for (auto& s : svcs) {
        if (s.command.empty()) {
            error = "service '" + s.name + "' has no cmd";
            return false;
        }
    }

    if (!order_services(svcs, error)) return false;
    out = svcs;
    return true;
}
