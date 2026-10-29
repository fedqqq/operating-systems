#pragma once

#include "config.hpp"

class Daemon final {
public:
    static Daemon& instance();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    int run(const std::filesystem::path& config_path,
            const std::filesystem::path& pid_path, Config config);

private:
    Daemon() = default;
};
