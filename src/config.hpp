#pragma once
#include "process.hpp"
#include <string>
#include <vector>

// Reads services from a file. Returns false if the file can't be opened
// or a line is badly formed (the reason is written to 'error').
bool load_config(const std::string& path, std::vector<Service>& out, std::string& error);
