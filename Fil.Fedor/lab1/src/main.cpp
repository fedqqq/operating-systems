#include "config.hpp"
#include "daemon.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    namespace fs = std::filesystem;
    ::openlog("fil-fedor-lab1", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    try {
        fs::path config = "lab1.conf";
        fs::path pid = "/tmp/fil-fedor-lab1-" + std::to_string(::geteuid()) + ".pid";
        bool have_config = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage: lab1 [config] [--pid-file path]\n";
                ::closelog();
                return 0;
            }
            if (arg == "--pid-file" && i + 1 < argc) {
                pid = argv[++i];
            } else if (!arg.empty() && arg.front() != '-' && !have_config) {
                config = arg;
                have_config = true;
            } else {
                throw std::runtime_error("Unknown or incomplete argument: " + arg);
            }
        }
        config = fs::absolute(config).lexically_normal();
        pid = fs::absolute(pid).lexically_normal();
        auto settings = load_config(config);
        const auto resolved_pid = fs::weakly_canonical(pid);
        if (resolved_pid == fs::canonical(config) || resolved_pid == fs::weakly_canonical(event_log_path))
            throw std::runtime_error("PID file must differ from config and event log");
        const int result = Daemon::instance().run(config, pid, settings);
        ::closelog();
        return result;
    } catch (const std::exception& error) {
        ::syslog(LOG_ERR, "Startup/operation failed: %s", error.what());
        std::cerr << "lab1: " << error.what() << '\n';
        ::closelog();
        return 1;
    }
}
