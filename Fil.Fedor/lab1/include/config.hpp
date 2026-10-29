#pragma once

#include <filesystem>
#include <vector>

struct Config {
    std::vector<std::filesystem::path> directories;
};

inline constexpr const char* event_log_path = "/var/log/fil-fedor-disk-monitor.log";
Config load_config(const std::filesystem::path& path);
