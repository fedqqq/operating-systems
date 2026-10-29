#pragma once

#include "config.hpp"
#include "unique_fd.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

class Watcher {
public:
    explicit Watcher(const Config& config);
    int fd() const { return fd_.get(); }
    bool consume(); // false means inotify overflowed and watches must be rebuilt
    void expire_moves();

private:
    struct Move {
        std::filesystem::path from;
        std::chrono::steady_clock::time_point time;
    };
    UniqueFd fd_;
    std::vector<std::filesystem::path> roots_;
    std::map<int, std::filesystem::path> watches_;
    std::map<std::uint32_t, Move> moves_;
    void add_tree(const std::filesystem::path& root);
    void remove_tree(const std::filesystem::path& root);
    void rename_tree(const std::filesystem::path& from, const std::filesystem::path& to);
};
