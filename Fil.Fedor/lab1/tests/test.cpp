#include "config.hpp"
#include "daemon.hpp"
#include "watcher.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/inotify.h>
#include <type_traits>
#include <unistd.h>

static_assert(!std::is_default_constructible_v<Daemon>);
static_assert(!std::is_copy_constructible_v<Daemon>);
static_assert(!std::is_copy_assignable_v<Daemon>);

namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    char name[] = "/tmp/fil-fedor-lab1-test-XXXXXX";
    if (!::mkdtemp(name)) return 1;
    const fs::path root(name);
    int result = 0;
    try {
        require(&Daemon::instance() == &Daemon::instance(), "Singleton identity failed");
        const auto nested = root / "watched" / "nested";
        fs::create_directories(nested);
        const auto config_path = root / "lab1.conf";
        std::ofstream(config_path) << "directory=watched\ndirectory=watched/nested\n";
        const auto config = load_config(config_path);
        require(config.directories.size() == 1, "Nested roots were not deduplicated");
        require(config.directories.front() == root / "watched", "Relative path resolution failed");

        Watcher watcher(config);
        watcher.consume(); // Drain directory scans before generating the test event.
        std::ofstream(nested / "example.txt") << "example\n";
        pollfd descriptor{watcher.fd(), POLLIN, 0};
        require(::poll(&descriptor, 1, 1000) == 1, "inotify did not receive a file event");
        alignas(inotify_event) char buffer[4096];
        const auto count = ::read(watcher.fd(), buffer, sizeof(buffer));
        require(count > 0, "Cannot read inotify events");
        bool created = false;
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
            if ((event->mask & IN_CREATE) && event->len && std::string(event->name) == "example.txt")
                created = true;
            offset += sizeof(inotify_event) + event->len;
        }
        require(created, "File creation in a nested directory was not reported");

        std::ofstream(config_path) << "directory=does-not-exist\n";
        bool rejected = false;
        try { load_config(config_path); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid configuration was accepted");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    fs::remove_all(root);
    return result;
}
