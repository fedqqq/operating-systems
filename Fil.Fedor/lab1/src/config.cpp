#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool contains(const fs::path& parent, const fs::path& child) {
    auto left = parent.begin();
    auto right = child.begin();
    for (; left != parent.end(); ++left, ++right) {
        if (right == child.end() || *left != *right) return false;
    }
    return true;
}
} // namespace

Config load_config(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open config: " + path.string());
    Config config;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != "directory")
            throw std::runtime_error("Expected directory=path");
        const auto value = trim(line.substr(separator + 1));
        if (value.empty()) throw std::runtime_error("Empty directory");
        auto directory = fs::path(value);
        if (directory.is_relative()) directory = path.parent_path() / directory;
        directory = fs::canonical(directory);
        if (!fs::is_directory(directory)) throw std::runtime_error("Not a directory: " + directory.string());
        if (contains(directory, event_log_path))
            throw std::runtime_error("Cannot watch the event log directory (logging feedback loop)");
        config.directories.push_back(directory);
    }
    if (input.bad()) throw std::runtime_error("Cannot read config");
    if (config.directories.empty()) throw std::runtime_error("At least one directory is required");
    std::sort(config.directories.begin(), config.directories.end());
    std::vector<fs::path> roots;
    for (const auto& directory : config.directories) {
        const auto covered = std::any_of(roots.begin(), roots.end(), [&](const auto& root) {
            return contains(root, directory);
        });
        if (!covered) roots.push_back(directory);
    }
    config.directories = std::move(roots);
    return config;
}
